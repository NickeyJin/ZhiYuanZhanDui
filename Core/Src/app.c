/*
app.c —— 主循环：PS2 手柄 → 麦轮底盘（+ 机械臂）

    app_init() : 初始化各模块
    app_task() : 每 20ms 一拍：读手柄 → 掉线/急停保护 → 4 方向解算 → 斜坡 → 底盘 → 灯

摇杆处理分三层，一层只干一件事：
    stick_axis()     先把死区抠掉，输出 -115 ~ +115（保留方向和幅度）
    axis_speed()     单轴幅度 → 速度（上下限分开给，自转轴的下限要比平移高）
    stick_dir4()     左摇杆两轴 → 只取主导轴 → vx/vy，四个方向各占 90°

*/
#include "main.h"
#include "app.h"
#include "arm.h"
#include "board.h"
#include "chassis.h"
#include "dwt_delay.h"
#include "motor.h"
#include "ps2.h"
#include "stm32f1xx_hal.h"
#include "tune.h"


/* 斜坡的当前值（三个自由度各一个） */
static int16_t ramp_vx, ramp_vy, ramp_w;

/* 摇杆单轴 → -115 ~ +115，正方向 = 右 / 下（PS2 的原始方向）。
   死区内的部分直接当 0；死区外的部分再减掉死区，
   这样"刚出死区"就能给到最小速度，不会有一段推了不动。 */
static int16_t stick_axis(uint8_t raw)
{
    int16_t d = (int16_t)raw - PS2_STICK_CENTER;

    if (d > -APP_STICK_DEADZONE && d < APP_STICK_DEADZONE) return 0;

    return (int16_t)((d > 0) ? (d - APP_STICK_DEADZONE) : (d + APP_STICK_DEADZONE));
}

/* 单轴幅度 → 速度：|d| = 1 时给 speed_min，|d| = 满量程时给 speed_max。
   符号跟着 d 走。*/
static int16_t axis_speed(int16_t d, int16_t speed_min, int16_t speed_max)
{
    const int32_t span = PS2_STICK_MAX - APP_STICK_DEADZONE;
    int32_t mag;

    if (d == 0) return 0;
    if (speed_min < 0) speed_min = 0;
    if (speed_min > speed_max) speed_min = speed_max;

    mag = (d < 0) ? -(int32_t)d : (int32_t)d;
    mag = speed_min + mag * ((int32_t)speed_max - speed_min) / span;
    if (mag > speed_max) mag = speed_max;

    return (int16_t)((d < 0) ? -mag : mag);
}

/*
左摇杆 → 只有 4 个方向：前 / 后 / 左移 / 右移，各占 90° 扇区。

怎么分扇区：把摇杆位置看成 (dx, dy) 两个分量，比较谁的绝对值大 ——
    |dx| >= |dy|  横向（左移 / 右移）
    |dx| <  |dy|  纵向（前进 / 后退）
这个"谁大听谁的"正好等价于以 45° 为界切出四个 90° 扇区，
而且斜推时只取主导轴，不会出现"斜着走"的中间态。

速度大小取主导轴的幅度，所以贴着 45° 推也能推满。
*/
static void stick_dir4(uint8_t lx, uint8_t ly, int16_t speed_max,
                       int16_t *vx, int16_t *vy)
{
    int16_t dx = stick_axis(lx);            /* > 0 = 向右推 */
    int16_t dy = stick_axis(ly);            /* > 0 = 向后推（PS2 下 = 0xFF） */
    int16_t ax = (dx < 0) ? (int16_t)(-dx) : dx;
    int16_t ay = (dy < 0) ? (int16_t)(-dy) : dy;

    *vx = 0;
    *vy = 0;
    if (ax == 0 && ay == 0) return;         /* 松手 / 死区内 */

    if (ax >= ay)
        *vy = axis_speed(dx, APP_SPEED_MIN, speed_max);              /* 横移：右推 = 右移 */
    else
        *vx = axis_speed((int16_t)(-dy), APP_SPEED_MIN, speed_max);  /* 前后：上推 = 前进 */
}

/* 一拍最多变化多少：从 0 加到满速大约 APP_RAMP_MS 毫秒 */
#define APP_RAMP_STEP   ((APP_SPEED_MAX * APP_LOOP_MS) / APP_RAMP_MS)

static int16_t ramp_to(int16_t cur, int16_t target)
{
    int32_t delta;

    /* 从静止起步时别一格一格地磨：直接给到 APP_SPEED_MIN。
       这颗电机占空比太低带不动，从 0 开始加法它就在那儿干耗不转。*/
    if (cur == 0 && target != 0)
    {
        int32_t t     = (target < 0) ? -(int32_t)target : (int32_t)target;
        int32_t start = APP_SPEED_MIN;

        if (start > t) start = t;
        return (int16_t)((target > 0) ? start : -start);
    }

    delta = (int32_t)target - (int32_t)cur;
    if (delta >  APP_RAMP_STEP) delta =  APP_RAMP_STEP;
    if (delta < -APP_RAMP_STEP) delta = -APP_RAMP_STEP;

    return (int16_t)((int32_t)cur + delta);
}

/* 三个自由度一起斜坡 */
static void ramp3(int16_t tgt_vx, int16_t tgt_vy, int16_t tgt_w)
{
    ramp_vx = ramp_to(ramp_vx, tgt_vx);
    ramp_vy = ramp_to(ramp_vy, tgt_vy);
    ramp_w  = ramp_to(ramp_w,  tgt_w);
}

/* ==================== 初始化 ==================== */

void app_init(void)
{
    dwt_init();         /* delay_us 要用 DWT，PS2 的时序全靠它 */
    ps2_init();

    tune_init();        /* 闪 3 下 + 恢复上次调好的速度/配平 */
    arm_init();         /* 软限位、上电初始位（爪子闭合的抓取姿态） */

    chassis_init();     /* 顺带把 8 路电机 PWM 起起来 */

    ps2_ok = 0;
    HAL_Delay(300);     /* 等电源稳一稳，再让电机上电 */

}

/* ==================== 主循环 ==================== */

/* 开车固件的一拍：读手柄 → 保护 → 解算 → 底盘 → 灯 */
static void app_drive_tick(void)
{
    int16_t vx, vy, w;
    int16_t speed_max, turn_max, turn_min;

    ps2_poll();
    tune_update();          /* 收统计 + 处理配平旋钮 */

    /* --- 掉线：全停 --- */
    if (!ps2_ok) {
        ramp_vx = ramp_vy = ramp_w = 0;
        chassis_stop();
        arm_stop();
        LED_OFF();
        return;
    }

    /* --- 机械臂：自己读按键（点动/录制），也负责 SELECT+START 松劲 --- */
    arm_task();

    /* --- 急停优先 --- */
    if (ps2_btn1 & PS2_BTN_START) {
        ramp_vx = ramp_vy = ramp_w = 0;
        chassis_stop();
        arm_stop();
        tune_led_slow();        /* 慢闪 = 急停中 */
        return;
    }

    /* --- 速度档位（按住 L1 全减半） --- */
    if (ps2_btn2 & PS2_BTN_L1) {
        speed_max = APP_SPEED_SLOW;
        turn_max  = (int16_t)(APP_TURN_MAX / 2);
        turn_min  = (int16_t)(APP_TURN_MIN / 2);
    } else {
        speed_max = APP_SPEED_MAX;
        turn_max  = APP_TURN_MAX;
        turn_min  = APP_TURN_MIN;
    }

    /* --- 左摇杆：4 个方向，只走一个轴 --- */
    stick_dir4(ps2_lx, ps2_ly, speed_max, &vx, &vy);

    /* --- 右摇杆左右：原地自转，右推 = 顺时针（omega < 0） --- */
    w = (int16_t)(-axis_speed(stick_axis(ps2_rx), turn_min, turn_max));

    /* --- 斜坡 + 下发 --- */
    ramp3(vx, vy, w);
    chassis_drive(ramp_vx, ramp_vy, ramp_w);

    tune_led();     /* 报 ID / 按键快闪 / 摇杆中闪 / 常亮 */
}

void app_task(void)
{
    static uint32_t last;

    if (HAL_GetTick() - last < APP_LOOP_MS) return;
    last = HAL_GetTick();

    app_drive_tick();
}
