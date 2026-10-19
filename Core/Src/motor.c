/*
motor.c —— 把一个 -1000 ~ +1000 的速度变成定时器两个通道上的 PWM

    speed_to_ccr()  : 速度 → CCR（满速 = 40% 占空比，见 motor.h）
    motor_write()   : 写 CCR，正负速度对应 IN1 / IN2 两路 PWM
    motor_set()     : 越界钳位 + 每轮微调 + 前后配平偏移 + 方向修正，然后写硬件
    motor_init()    : 上电把 8 路 PWM 起起来，先让轮子老实待着
    motor_stop_all(): 四轮滑行
    motor_brake_all(): 四轮刹车（IN1 = IN2 = 1）
*/
#include "main.h"
#include "motor.h"

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;

typedef struct
{
    TIM_HandleTypeDef *htim;
    uint32_t ch_in1;
    uint32_t ch_in2;
    uint8_t  invert;   /* 1 = 这个轮子的电机线接反了，软件帮它翻正 */
} MotorMap_t;

/* 下标 0 占位，让轮号 = 数组下标，看着顺眼也少一次减法 */
static const MotorMap_t motor_map[5] =
{
    { NULL   , 0             ,   0             , 0 },
    { &htim2 , TIM_CHANNEL_1 ,   TIM_CHANNEL_2 , 0 },   /* 1 左前 FL: PA0 / PA1  */
    { &htim2 , TIM_CHANNEL_3 ,   TIM_CHANNEL_4 , 1 },   /* 2 右前 FR: PA2 / PA3  */
    { &htim3 , TIM_CHANNEL_3 ,   TIM_CHANNEL_4 , 1 },   /* 3 左后 RL: PB0 / PB1  */
    { &htim1 , TIM_CHANNEL_1 ,   TIM_CHANNEL_4 , 0 },   /* 4 右后 RR: PA8 / PA11 */
};

/* 每轮微调，数值在 motor.h 里改。下标同样 0 占位 */
static const int16_t motor_trim[5] =
{
    0,
    MOTOR_TRIM_FL,
    MOTOR_TRIM_FR,
    MOTOR_TRIM_RL,
    MOTOR_TRIM_RR,
};

/* 运行期配平偏移：前一对 +d、后一对 -d。
   开车固件里用 SELECT + ↑/↓ 实时调，正常烧进去之后就是 0 或上次存的值。 */
static int16_t motor_trim_delta;

void motor_set_trim_delta(int16_t delta)
{
    if (delta >  300) delta =  300;
    if (delta < -300) delta = -300;
    motor_trim_delta = delta;
}

int16_t motor_get_trim_delta(void)
{
    return motor_trim_delta;
}

/* 全局速度倍率（千分比）：1000 = 不缩放。开车固件里 SELECT+←/→ 实时调 */
static int16_t motor_speed_scale = 1000;

/* 每轮附加偏移（千分比）：下标 = 轮号，0 = 不补。
   用来补"某个轮子明显比别人慢"这种机械问题，别拿它当永久配平 */
static int16_t motor_wheel_delta[5];

/* 起步踹一脚用：每轮记"本轮起步时刻"和"上一拍的输出速度" */
static uint32_t motor_kick_t0[5];
static int16_t  motor_kick_last[5];

void motor_set_speed_scale(int16_t scale)
{
    if (scale < 100)  scale = 100;
    if (scale > 1250) scale = 1250;
    motor_speed_scale = scale;
}

int16_t motor_get_speed_scale(void)
{
    return motor_speed_scale;
}

void motor_set_wheel_delta(uint8_t wheel, int16_t delta)
{
    if (wheel < MOTOR_FL || wheel > MOTOR_RR) return;
    if (delta >  600) delta =  600;
    if (delta < -600) delta = -600;
    motor_wheel_delta[wheel] = delta;
}

int16_t motor_get_wheel_delta(uint8_t wheel)
{
    if (wheel < MOTOR_FL || wheel > MOTOR_RR) return 0;
    return motor_wheel_delta[wheel];
}

/* 速度 → CCR。满速 MOTOR_SPEED_MAX 对应 MOTOR_PWM_CCR_MAX（40% 占空比） */
static uint32_t speed_to_ccr(int16_t speed)
{
    uint32_t mag = (speed < 0) ? (uint32_t)(-speed) : (uint32_t)speed;

    if (mag > (uint32_t)MOTOR_SPEED_MAX) mag = (uint32_t)MOTOR_SPEED_MAX;

    return mag * MOTOR_PWM_CCR_MAX / (uint32_t)MOTOR_SPEED_MAX;
}

static void motor_write(const MotorMap_t *m, int16_t speed)
{
    if (speed > 0)
    {
        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in1, speed_to_ccr(speed));
        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in2, 0);
    }
    else if (speed < 0)
    {
        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in1, 0);
        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in2, speed_to_ccr(speed));
    }
    else
    {
        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in1, 0);
        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in2, 0);
    }
}

void motor_init(void)
{
    /* TIM2：左前 + 右前，4 路全开 */
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);

    /* TIM3：左后 2 路 */
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);

    /* TIM1：右后 2 路 */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4);

    motor_stop_all();   /* 上电先让轮子老实待着 */
}

void motor_set(uint8_t wheel, int16_t speed)
{
    const MotorMap_t *m;
    int16_t trim;

    if (wheel < MOTOR_FL || wheel > MOTOR_RR) return;
    m = &motor_map[wheel];

    if (speed >  MOTOR_SPEED_MAX) speed =  MOTOR_SPEED_MAX;
    if (speed < -MOTOR_SPEED_MAX) speed = -MOTOR_SPEED_MAX;

    /* ① 先乘全局速度倍率：这一层先缩，后面单轮补偿才有往上加的余量 */
    speed = (int16_t)(((int32_t)speed * motor_speed_scale) / MOTOR_TRIM_BASE);
    if (speed >  MOTOR_SPEED_MAX) speed =  MOTOR_SPEED_MAX;
    if (speed < -MOTOR_SPEED_MAX) speed = -MOTOR_SPEED_MAX;

    /* ② 每轮微调 = 出厂值 + 前后配平偏移 ± + 单轮附加偏移（补某个轮子偏慢） */
    trim = motor_trim[wheel];
    if (motor_trim_delta != 0)
    {
        trim = (int16_t)(trim + (((wheel == MOTOR_FL) || (wheel == MOTOR_FR))
                                 ? motor_trim_delta : (int16_t)(-motor_trim_delta)));
    }
    trim = (int16_t)(trim + motor_wheel_delta[wheel]);
    if (trim < 400)  trim = 400;
    if (trim > 1600) trim = 1600;

    speed = (int16_t)(((int32_t)speed * trim) / MOTOR_TRIM_BASE);
    if (speed >  MOTOR_SPEED_MAX) speed =  MOTOR_SPEED_MAX;
    if (speed < -MOTOR_SPEED_MAX) speed = -MOTOR_SPEED_MAX;

    /* ③ 起步踹一脚：从静止开始转、或者反向的那一瞬间，静摩擦最大，
          先给一段短促的高电平把轮子推起来，过了 MOTOR_KICK_MS 自动回落。
          只对"指令幅度够大"的情况生效，轻轻推摇杆不会一顿一顿的。 */
#if (MOTOR_KICK_MS > 0)
    {
        uint32_t now = HAL_GetTick();

        /* 上一拍是停的，或者方向反了 → 重新计一次起步时刻 */
        if ((speed != 0) &&
            ((motor_kick_last[wheel] == 0) ||
             ((speed > 0) != (motor_kick_last[wheel] > 0))))
        {
            motor_kick_t0[wheel] = now;
        }
        motor_kick_last[wheel] = speed;

        if (((speed >= MOTOR_KICK_MIN) || (speed <= -MOTOR_KICK_MIN)) &&
            ((uint32_t)(now - motor_kick_t0[wheel]) < (uint32_t)MOTOR_KICK_MS))
        {
            int32_t boosted = ((int32_t)speed * MOTOR_KICK_GAIN) / 100;

            if (boosted >  MOTOR_SPEED_MAX) boosted =  MOTOR_SPEED_MAX;
            if (boosted < -MOTOR_SPEED_MAX) boosted = -MOTOR_SPEED_MAX;
            speed = (int16_t)boosted;
        }
    }
#else
    motor_kick_last[wheel] = speed;
#endif

    if (m->invert) speed = (int16_t)(-speed);

    motor_write(m, speed);
}

void motor_stop_all(void)
{
    for (uint8_t w = MOTOR_FL; w <= MOTOR_RR; w++) motor_set(w, 0);
}

void motor_brake_all(void)
{
    /* IN1 = IN2 都拉满：DRV8833 上下桥同时导通 = 刹车 */
    for (uint8_t w = MOTOR_FL; w <= MOTOR_RR; w++)
    {
        const MotorMap_t *m = &motor_map[w];

        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in1, MOTOR_PWM_COUNTS);
        __HAL_TIM_SET_COMPARE(m->htim, m->ch_in2, MOTOR_PWM_COUNTS);
    }
}
