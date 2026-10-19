/*
tune.c —— 现场调参（麦轮底盘）

    tune_init()    上电：闪 3 下、恢复上次调好的值（第一次上电用下面的默认值）
    tune_update()  每 20ms：处理两个现场旋钮
    tune_led()     灯语：按键快闪 / 摇杆中闪 / 常亮

为什么单独一个文件：这些代码全是"调车时才用"的，塞进 app.c 会把真正的主循环
逻辑埋掉，谁看都得找半天。开车的主线在 app.c，这里只负责调参和灯。

保存原理：调好的值放在 .persist 段（链接脚本里专门划的一段，复位不清零、
只有断电才丢）。所以现场调完不用管，只要不拔电就一直记着；想永久保存就
用 read_vars.ps1 把值读出来，抄进下面的 defaults_init()，重新编译烧录。
*/
#include "tune.h"
#include "app.h"
#include "board.h"
#include "motor.h"
#include "ps2.h"
#include "stm32f1xx_hal.h"

#define TUNE_PERSIST  __attribute__((section(".persist"), used))

/* 布局版本号：变量增删了就把这个数换掉，让上次留下的脏数据自动作废 */
#define TUNE_MAGIC    0x54554E32u

TUNE_PERSIST uint32_t t_persist_magic; /* == TUNE_MAGIC 才认为下面的数据有效 */
TUNE_PERSIST int32_t  t_trim_delta;    /* 前后配平偏移：前一对 +d、后一对 -d */
TUNE_PERSIST int32_t  t_speed_scale;   /* 总速度倍率（千分比，100~1250，1000 = 原样） */
TUNE_PERSIST int32_t  t_wheel_delta[4];/* 四个轮子的附加偏移（千分比，±600）：0=FL 1=FR 2=RL 3=RR */

/* 固件指纹：读到 0x4D454331（"MEC1"）才说明板上跑的是开车固件。
   volatile 是必须的——否则 -O2 会把"只写不读"的变量当死代码优化掉。 */
TUNE_PERSIST volatile uint32_t t_fw_id;

/* ==================== 默认值 ==================== */

/* 第一次上电（或刚烧完固件）才写默认值，复位不清 —— 现场调好的值不会被覆盖。
   下面这几个数是 2026-10-19 22:30 现场调好的，固化成上电起点；
   正式比赛时还可以用旋钮现场再调。 */
static void defaults_init(void)
{
    if (t_persist_magic != TUNE_MAGIC)
    {
        t_persist_magic  = TUNE_MAGIC;
        t_trim_delta     = 30;     /* 前一对比后一对快 3% */
        t_speed_scale    = 960;    /* 总速度 96% */
        t_wheel_delta[0] = 80;     /* FL +8%  */
        t_wheel_delta[1] = 40;     /* FR +4%  */
        t_wheel_delta[2] = 200;    /* RL +20% */
        t_wheel_delta[3] = 140;    /* RR +14% */
    }
}

/* ==================== 小工具 ==================== */

/* 摇杆偏离中位的幅度是否超过死区 */
static uint8_t axis_moved(uint8_t raw)
{
    int16_t d = (int16_t)raw - (int16_t)PS2_STICK_CENTER;
    if (d < 0) d = (int16_t)(-d);
    return (d > APP_STICK_DEADZONE) ? 1U : 0U;
}

/* 有没有任何一个输入（按钮或摇杆偏移）*/
static uint8_t input_active(void)
{
    if (ps2_btn1 || ps2_btn2) return 1;
    if (axis_moved(ps2_lx) || axis_moved(ps2_ly)) return 1;
    if (axis_moved(ps2_rx) || axis_moved(ps2_ry)) return 1;
    return 0;
}

/* ==================== 旋钮①：前后配平 ==================== */

/*
按住 SELECT 不放，再按 ↑ / ↓ 调"前一对 vs 后一对"的配速差。
    ↑ = 前一对相对变快（d += step）    ↓ = 前一对相对变慢（d -= step）
每 250ms 走一格，一格 APP_TRIM_STEP，按住 L1 时一格 20（快速走），范围 ±300。
用途：横移画圈时边移边调，左移时车头顺时针转就按 ↑、逆时针转就按 ↓，
调到横移不自己转为止。调好以后把 t_trim_delta 抄进 defaults_init()。
*/
static void trim_adjust(void)
{
    static uint32_t t_last_step;
    uint32_t now  = HAL_GetTick();
    uint8_t  sel  = (ps2_btn1 & PS2_BTN_SELECT) ? 1U : 0U;
    uint8_t  up   = (ps2_btn1 & PS2_BTN_UP)     ? 1U : 0U;
    uint8_t  down = (ps2_btn1 & PS2_BTN_DOWN)   ? 1U : 0U;
    int16_t  d, step;

    if (!sel || (!up && !down) || (ps2_btn1 & PS2_BTN_START)) { t_last_step = now - 250U; return; }
    if ((uint32_t)(now - t_last_step) < 250U) return;
    t_last_step = now;

    step = (ps2_btn2 & PS2_BTN_L1) ? 20 : APP_TRIM_STEP;   /* 按住 L1 快走 */

    d = (int16_t)t_trim_delta;
    d = (int16_t)(d + (up ? step : -step));
    if (d >  APP_TRIM_LIMIT) d =  APP_TRIM_LIMIT;
    if (d < -APP_TRIM_LIMIT) d = -APP_TRIM_LIMIT;

    t_trim_delta = (int32_t)d;
    motor_set_trim_delta(d);

    LED_OFF();          /* 每走一格闪一下，当作"哔"的反馈 */
    HAL_Delay(40);
    LED_ON();
}

/* ==================== 旋钮②：总速度 / 单轮补偿 ==================== */

/*
按住 SELECT 不放，再按 ← / →，调哪一路看**肩上键**：

    SELECT + ← / →              总速度（四个轮子一起）   → 快  ← 慢
    SELECT + L2 + ← / →         左前轮 FL
    SELECT + L1 + ← / →         左后轮 RL
    SELECT + R2 + ← / →         右前轮 FR
    SELECT + R1 + ← / →         右后轮 RR

    L2/L1 = 左边两个轮子，R2/R1 = 右边两个轮子，一眼能对上。
    一格 2%（20‰）。

范围：总速度 100~1250‰（1000 = 原样）；单轮 -600~+600‰。
每一档速度对应的最佳配平不一样，所以这个功能在正式版里保留，赛场上可以随时调。
*/
#define TUNE_SCALE_MIN      100
#define TUNE_SCALE_MAX      1250
#define TUNE_DELTA_MIN      (-600)
#define TUNE_DELTA_MAX      600
#define TUNE_ADJ_STEP       20      /* 一格 20‰ = 2% */

static void speed_adjust(void)
{
    static uint32_t t_last_step;
    uint32_t now = HAL_GetTick();
    uint8_t  sel = (ps2_btn1 & PS2_BTN_SELECT) ? 1U : 0U;
    uint8_t  lft = (ps2_btn1 & PS2_BTN_LEFT)   ? 1U : 0U;
    uint8_t  rgt = (ps2_btn1 & PS2_BTN_RIGHT)  ? 1U : 0U;
    uint8_t  wheel = 0;     /* 0 = 总速度；1~4 = 轮号（FL/FR/RL/RR） */
    int16_t  v, step;

    /* 没按 SELECT、没按左右、或者同时按了左右 → 什么都不做 */
    if (!sel || (lft == rgt) || (ps2_btn1 & PS2_BTN_START))
    {
        t_last_step = now - 250U;
        return;
    }
    if ((uint32_t)(now - t_last_step) < 250U) return;
    t_last_step = now;

    /* 肩上键决定调哪一路；四个都不按 = 总速度 */
    if      (ps2_btn2 & PS2_BTN_L2) wheel = MOTOR_FL;
    else if (ps2_btn2 & PS2_BTN_L1) wheel = MOTOR_RL;
    else if (ps2_btn2 & PS2_BTN_R2) wheel = MOTOR_FR;
    else if (ps2_btn2 & PS2_BTN_R1) wheel = MOTOR_RR;

    step = TUNE_ADJ_STEP;

    if (wheel == 0)
    {
        v = (int16_t)t_speed_scale;
        v = (int16_t)(v + (rgt ? step : -step));
        if (v < TUNE_SCALE_MIN) v = TUNE_SCALE_MIN;
        if (v > TUNE_SCALE_MAX) v = TUNE_SCALE_MAX;
        t_speed_scale = (int32_t)v;
        motor_set_speed_scale(v);
    }
    else
    {
        v = (int16_t)t_wheel_delta[wheel - 1];
        v = (int16_t)(v + (rgt ? step : -step));
        if (v < TUNE_DELTA_MIN) v = TUNE_DELTA_MIN;
        if (v > TUNE_DELTA_MAX) v = TUNE_DELTA_MAX;
        t_wheel_delta[wheel - 1] = (int32_t)v;
        motor_set_wheel_delta(wheel, v);
    }

    LED_OFF();          /* 每走一格闪一下，当"哔"的反馈 */
    HAL_Delay(40);
    LED_ON();
}

/* ==================== 对外接口 ==================== */

void tune_init(void)
{
    uint8_t i;

    /* 上电签名：快闪 3 下 */
    for (i = 0; i < 3; i++)
    {
        LED_ON();  HAL_Delay(120);
        LED_OFF(); HAL_Delay(120);
    }

    defaults_init();                                /* 首次上电/断电后写默认值，复位不覆盖 */
    motor_set_trim_delta((int16_t)t_trim_delta);    /* 恢复上次调好的前后配平 */
    motor_set_speed_scale((int16_t)t_speed_scale);  /* 恢复总速度 */
    for (i = 0; i < 4; i++)                         /* 恢复四个轮子的单轮补偿 */
        motor_set_wheel_delta((uint8_t)(MOTOR_FL + i), (int16_t)t_wheel_delta[i]);

    t_fw_id = 0x4D454331;   /* "MEC1"：读到它才说明烧的是开车固件 */
}

void tune_update(void)
{
    trim_adjust();
    speed_adjust();
}

void tune_led(void)
{
    /* 按按钮 = 快闪 / 只推摇杆 = 中闪 / 没碰 = 常亮 */
    if (ps2_btn1 || ps2_btn2)  led_fast();
    else if (input_active())   led_mid();
    else                       LED_ON();
}

void tune_led_slow(void)
{
    led_slow();         /* 300ms 一翻：急停中 */
}
