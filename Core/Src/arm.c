/*
arm.c —— 机械臂：手动点动 + 上电初始位 + 放块宏（R2+R1）

    两层东西分清楚，别混：
        1) t_arm_lo / t_arm_hi[]  软限位（实测值内收 10us），所有脉宽都过这一关
        2) t_arm_tgt[]/now[]      目标 / 当前，每 20ms 朝目标挪一小步（速度见 arm.h）

*/
#include "main.h"
#include "app.h"
#include "arm.h"
#include "board.h"
#include "ps2.h"
#include "stm32f1xx_hal.h"

extern TIM_HandleTypeDef htim4;

#define ARM_PERSIST   __attribute__((section(".persist"), used))


/* 边沿检测：同一个按键只在"刚按下"那一拍返回 1。
   用 GCC 的语句表达式写，省得给每个键都开一个 static 变量。*/
#define EDGE(cond) ({ static uint8_t _last_; uint8_t _n_ = (cond) ? 1u : 0u; \
                      uint8_t _e_ = (_n_ && !_last_) ? 1u : 0u; _last_ = _n_; _e_; })

#define ARM_MAGIC       0x41524D34u     /* "ARM4" .persist 有效（2026-10-19 布局变了，抬版本号强制重新初始化） */
/* ===================== 限位 / 速度 ===================== */
/* 出厂默认值来自实测（各内收 10us）。软限位是编译期常量，运行中不再改：
   要调就直接改 arm.h 里的 ARM_US_* 再重新编译。 */
static const uint16_t arm_lo_def[ARM_CH_N] = { ARM_US_SH_MIN, ARM_US_EL_MIN, ARM_US_WR_MIN, ARM_US_GR_MIN };
static const uint16_t arm_hi_def[ARM_CH_N] = { ARM_US_SH_MAX, ARM_US_EL_MAX, ARM_US_WR_MAX, ARM_US_GR_MAX };
static const uint16_t arm_sp[ARM_CH_N] = { ARM_SPEED_SH,  ARM_SPEED_EL,  ARM_SPEED_WR,  ARM_SPEED_GR  };

/* 硬边界：再宽也不许超出舵机本身能吃的脉宽 */
#define ARM_HARD_MIN    500
#define ARM_HARD_MAX   2500

/* ===================== 变量 ===================== */
/* 当前/目标脉宽：.persist 段，复位不丢、断电丢（上电后 arm_init 再夹一遍限位） */
ARM_PERSIST uint16_t t_arm_now[ARM_CH_N];
ARM_PERSIST uint16_t t_arm_tgt[ARM_CH_N];
ARM_PERSIST uint32_t t_arm_magic;

/* 生效的软限位：arm_init 里从 arm_lo_def/arm_hi_def 拷过来，运行中不再改 */
static uint16_t t_arm_lo[ARM_CH_N];
static uint16_t t_arm_hi[ARM_CH_N];

/* ===================== 内部状态 ===================== */
static uint8_t  pwr_on;

/* 灯：短闪 n 下的状态机（不占用主循环时间） */
static uint8_t  sig_n;
static uint8_t  sig_level;
static uint32_t sig_t;

/* ===================== 基础：写脉宽 / 上电 ===================== */
static void arm_write(uint8_t i, uint16_t us)
{
    if (us < t_arm_lo[i]) us = t_arm_lo[i];
    if (us > t_arm_hi[i]) us = t_arm_hi[i];
    t_arm_now[i] = us;

    switch (i) {
    case 0: __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, us); break;
    case 1: __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_2, us); break;
    case 2: __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, us); break;
    default:__HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_4, us); break;
    }
}

/* 松劲 = 停止输出脉冲，舵机不受力，可以用手掰 */
static void arm_power(uint8_t on)
{
    if (on == pwr_on) return;
    pwr_on = on;

    if (on) {
        HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);
        HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_2);
        HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_3);
        HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_4);
    } else {
        HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_1);
        HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_2);
        HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_3);
        HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_4);
    }
}

static void arm_set_tgt(uint8_t i, uint16_t us)
{
    if (us < t_arm_lo[i]) us = t_arm_lo[i];
    if (us > t_arm_hi[i]) us = t_arm_hi[i];
    t_arm_tgt[i] = us;
}

/* ===================== 灯 ===================== */
static void sig(uint8_t n)
{
    sig_n = (uint8_t)(n * 2u);
    sig_level = 0;
    sig_t = HAL_GetTick();
}

static uint8_t sig_busy(void)
{
    if (!sig_n) return 0;

    if (HAL_GetTick() - sig_t >= (sig_level ? 80u : 120u)) {
        sig_t = HAL_GetTick();
        sig_level = sig_level ? 0u : 1u;
        if (sig_level) { LED_ON();  sig_n--; }
        else           { LED_OFF(); }
    }
    return 1;
}

/* START 是整车急停：按下时机械臂也不许自己动（但 SELECT+START 那个松劲组合要放过） */
static uint8_t arm_estop(void)
{
    return ((ps2_btn1 & PS2_BTN_START) && !(ps2_btn1 & PS2_BTN_SELECT)) ? 1u : 0u;
}

/* ===================== 挪动：每拍朝目标走一小步 ===================== */
static void arm_move_tick(void)
{
    uint8_t i;

    for (i = 0; i < ARM_CH_N; i++) {
        int32_t d    = (int32_t)t_arm_tgt[i] - (int32_t)t_arm_now[i];
        int32_t step = (int32_t)arm_sp[i] * APP_LOOP_MS / 1000;

        if (step < 1) step = 1;
        if (d >  step) d =  step;
        if (d < -step) d = -step;

        if (d) arm_write(i, (uint16_t)((int32_t)t_arm_now[i] + d));
    }
}

/* ===================== 上电初始位 / 放块宏 ===================== */
/* 「放块宏」= 按下 R2+R1 之后自动跑完的一整套动作：
     1) 记下按下瞬间四路的姿态（put_back[]）
     2) 肩/肘/腕 走到放块位（ARM_US_PUT_*，见 arm.h）
     3) 等这三路都到位（距目标 ≤ ARM_PUT_ARRIVE_US）之后才张开爪子，
        保持 ARM_PUT_HOLD_MS 让方块落稳
     4) 四路回到 put_back[] —— 也就是按下 R2+R1 之前的舵机状态
   全程由 put_tick() 每拍推进一步，人手不用管；松劲或急停会立刻中止。 */
#define PUT_IDLE    0
#define PUT_MOVE    1
#define PUT_OPEN    2
#define PUT_BACK    3
#define PUT_MOVE_CH 3       /* 前三个舵机：肩 / 肘 / 腕 */

static uint8_t  put_st;              /* 宏当前阶段 */
static uint16_t put_back[ARM_CH_N];  /* 按下 R2+R1 前的四路姿态 */
static uint32_t put_t;               /* 当前阶段的计时起点 */

/* 前 n 路是否都到目标了 */
static uint8_t put_arrived(uint8_t n)
{
    uint8_t i, ok = 1;
    for (i = 0; i < n; i++) {
        int32_t d = (int32_t)t_arm_tgt[i] - (int32_t)t_arm_now[i];
        if (d < 0) d = -d;
        if (d > ARM_PUT_ARRIVE_US) ok = 0;
    }
    return ok;
}

static void put_start(void)
{
    uint8_t i;

    if (put_st != PUT_IDLE) return;     /* 正在跑，不重入 */
    if (!pwr_on) { sig(3); return; }    /* 松劲状态：先 SELECT+START 上电 */
    if (arm_estop()) return;

    for (i = 0; i < ARM_CH_N; i++) put_back[i] = t_arm_now[i];

    arm_set_tgt(0, ARM_US_PUT_SH);
    arm_set_tgt(1, ARM_US_PUT_EL);
    arm_set_tgt(2, ARM_US_PUT_WR);
    put_st = PUT_MOVE;
    put_t  = HAL_GetTick();
    sig(2);                             /* 闪 2 下 = 开始放块 */
}

static void put_tick(void)
{
    uint8_t i;

    if (put_st == PUT_IDLE) return;
    if (!pwr_on || arm_estop()) { put_st = PUT_IDLE; return; }   /* 松劲 / 急停：中止 */

    switch (put_st) {
    case PUT_MOVE:
        if (put_arrived(PUT_MOVE_CH) || (HAL_GetTick() - put_t > ARM_PUT_TIMEOUT_MS)) {
            arm_set_tgt(3, ARM_US_PUT_GR_OPEN);     /* 三路到位之后才松爪 */
            put_st = PUT_OPEN;
            put_t  = HAL_GetTick();
        }
        break;

    case PUT_OPEN:
        if (HAL_GetTick() - put_t >= ARM_PUT_HOLD_MS) {
            for (i = 0; i < ARM_CH_N; i++) arm_set_tgt(i, put_back[i]);   /* 原样回去 */
            put_st = PUT_BACK;
            put_t  = HAL_GetTick();
        }
        break;

    case PUT_BACK:
        if (put_arrived(ARM_CH_N) || (HAL_GetTick() - put_t > ARM_PUT_TIMEOUT_MS)) {
            put_st = PUT_IDLE;
            sig(1);                                 /* 闪 1 下 = 放块完成 */
        }
        break;

    default:
        put_st = PUT_IDLE;
        break;
    }
}

/* ===================== 手动点动 ===================== */
/* 一个键按住就往正/负方向一直走，方向固定写在 arm.h 的键位说明里 */
static void jog(uint8_t j, uint8_t up_key, uint8_t dn_key, uint16_t base_speed)
{
    int32_t step = (int32_t)base_speed * APP_LOOP_MS / 1000;
    int32_t tgt;
    int32_t sgn = 0;

    if (step < 1) step = 1;

    if (up_key) sgn =  1;
    if (dn_key) sgn = -1;
    if (!sgn) return;

    tgt = (int32_t)t_arm_tgt[j] + sgn * step;
    if (tgt < (int32_t)t_arm_lo[j]) tgt = (int32_t)t_arm_lo[j];
    if (tgt > (int32_t)t_arm_hi[j]) tgt = (int32_t)t_arm_hi[j];
    t_arm_tgt[j] = (uint16_t)tgt;
}

static void manual_tick(void)
{
    uint8_t fast = (ps2_btn2 & PS2_BTN_R1) ? 1u : 0u;
    uint8_t slow = (ps2_btn2 & PS2_BTN_L1) ? 1u : 0u;
    uint16_t k   = 1;      /* 速度倍率：1/3、1、3 */

    if (arm_estop()) return;
    if (ps2_btn1 & PS2_BTN_SELECT) return;   /* SELECT = 底盘调参键，按住时机械臂别跟着动 */

    if (slow) k = 0;                       /* 0 表示 1/3 */
    else if (fast) k = 3;

    /* ↑/↓ 肩, ←/→ 肘, △/× 腕, □/○ 爪 */
    /* 肩：脉宽变大 = 往下走，所以把 ↑/↓ 对调，让按 ↑ 是抬起来 */
    jog(0, (ps2_btn1 & PS2_BTN_DOWN) ? 1 : 0, (ps2_btn1 & PS2_BTN_UP)   ? 1 : 0,
        (uint16_t)(k ? ARM_SPEED_SH * k : ARM_SPEED_SH / 3));
    jog(1, (ps2_btn1 & PS2_BTN_RIGHT) ? 1 : 0, (ps2_btn1 & PS2_BTN_LEFT) ? 1 : 0,
        (uint16_t)(k ? ARM_SPEED_EL * k : ARM_SPEED_EL / 3));
    jog(2, (ps2_btn2 & PS2_BTN_TRIANGLE) ? 1 : 0, (ps2_btn2 & PS2_BTN_CROSS) ? 1 : 0,
        (uint16_t)(k ? ARM_SPEED_WR * k : ARM_SPEED_WR / 3));
    jog(3, (ps2_btn2 & PS2_BTN_CIRCLE) ? 1 : 0, (ps2_btn2 & PS2_BTN_SQUARE) ? 1 : 0,
        (uint16_t)(k ? ARM_SPEED_GR * k : ARM_SPEED_GR / 3));
}

/* ===================== 初始化 ===================== */
void arm_init(void)
{
    uint8_t i;
    uint8_t fresh;

    pwr_on = 0;
    arm_power(0);

    fresh = (t_arm_magic != ARM_MAGIC) ? 1u : 0u;

    if (fresh) {
        /* 第一次上电或者刚烧完固件：.persist 段不参与清零，里面是随机值，
           所以这一整块都得手动重置，不能只重置位置。 */
        t_arm_now[0] = 1348;    /* 肩：行程中间 */
        t_arm_now[1] = 1150;    /* 肘：微微弯 */
        t_arm_now[2] = 1500;    /* 腕：中间 */
        t_arm_now[3] = 1000;    /* 爪：半开 */

        t_arm_magic  = ARM_MAGIC;
    }

    /* 限位：每次启动都从 arm.h 的默认值拷一份，再夹一遍硬边界 */
    for (i = 0; i < ARM_CH_N; i++) {
        t_arm_lo[i] = arm_lo_def[i];
        t_arm_hi[i] = arm_hi_def[i];
        if (t_arm_lo[i] < ARM_HARD_MIN) t_arm_lo[i] = ARM_HARD_MIN;
        if (t_arm_hi[i] > ARM_HARD_MAX) t_arm_hi[i] = ARM_HARD_MAX;
        if (t_arm_hi[i] < (uint16_t)(t_arm_lo[i] + 40)) t_arm_hi[i] = (uint16_t)(t_arm_lo[i] + 40);
    }

    for (i = 0; i < ARM_CH_N; i++) {
        if (t_arm_now[i] < t_arm_lo[i]) t_arm_now[i] = t_arm_lo[i];
        if (t_arm_now[i] > t_arm_hi[i]) t_arm_now[i] = t_arm_hi[i];
        t_arm_tgt[i] = t_arm_now[i];
        arm_write(i, t_arm_now[i]);     /* 先写比较值，再开输出，避免先跳到别处 */
    }

    /* 闪三下提醒："马上要上电了，手放开"。然后保持松劲 ——
       舵机通电会去追 CCR 里的位置，手还按在臂上会被扭到。
       所以上电这件事交给 SELECT+START，你摆好了再按。 */
    {
        volatile uint8_t n;
        for (n = 0; n < 3; n++) {
            LED_ON();  HAL_Delay(120);
            LED_OFF(); HAL_Delay(120);
        }
    }
}

void arm_stop(void)
{
    uint8_t i;
    for (i = 0; i < ARM_CH_N; i++) t_arm_tgt[i] = t_arm_now[i];
}

/* ===================== 主任务（节拍由 app.c 的 20ms 驱动） ===================== */
void arm_task(void)
{
    uint8_t r2 = (ps2_btn2 & PS2_BTN_R2) ? 1u : 0u;

    /* ---- SELECT + START：四路松劲 / 恢复（掰机械臂用这个）---- */
    if ((ps2_btn1 & PS2_BTN_SELECT) && EDGE(ps2_btn1 & PS2_BTN_START)) {
        arm_power(pwr_on ? 0u : 1u);
        put_st = PUT_IDLE;                  /* 不管上电还是松劲，先把放块宏停掉 */
        if (pwr_on) {
            /* 上电：四路走到"初始位"（爪子闭合），也就是开机后该待的位置 */
            arm_set_tgt(0, ARM_US_INIT_SH);
            arm_set_tgt(1, ARM_US_INIT_EL);
            arm_set_tgt(2, ARM_US_INIT_WR);
            arm_set_tgt(3, ARM_US_INIT_GR);
        } else {
            arm_stop();                     /* 松劲：目标收回当前位，掰完再上电不会猛冲 */
        }
        sig(pwr_on ? 1u : 2u);
    }

    /* ---- 放块宏进行中：不接受任何手动操作，只推进宏本身 ----
         （要中止就按 SELECT+START 松劲，put_tick() 会立刻退出） */
    if (put_st != PUT_IDLE) {
        put_tick();
        arm_move_tick();
        if (!sig_busy()) LED_OFF();
        return;
    }

    /* ---- 按住 R2：只留放块宏这一个键 ---- */
    if (r2) {
        /* ---- 放块宏：R2+R1 一按到底（动作序列见 arm.h 顶部）----
              记下当前姿态 → 肩/肘/腕走到放块位 → 到位后松爪丢块
              → 保持一下 → 四路回到按下前的姿态 */
        if (EDGE(ps2_btn2 & PS2_BTN_R1)) {
            put_start();
        }
    }
    /* ---- 都不按：手动点动 ---- */
    else {
        manual_tick();
    }

    /* 短闪状态机：没在闪就把灯交回去 */
    if (!sig_busy()) LED_OFF();

    arm_move_tick();
}
