/*
chassis.c —— vx / vy / omega 三个自由度 → 四个轮子的速度 → 写进 motor.c

    chassis_drive()  : 标准麦轮逆运动学（公式见 chassis.h）
    chassis_stop()   : 四轮滑行停车
    chassis_forward / backward / turn_left / turn_right / strafe_left / strafe_right
    chassis_wheel()  : 单轮测试（上电自检用）
    chassis_init()   : 初始化，顺手把 motor.c 的 PWM 起起来
*/
#include "main.h"
#include "chassis.h"
#include "motor.h"

static int32_t iabs32(int32_t v) { return (v < 0) ? -v : v; }

void chassis_init(void)
{
    motor_init();
}

void chassis_drive(int16_t vx, int16_t vy, int16_t omega)
{
    int32_t v = (int32_t)vy * CHASSIS_STRAFE_SIGN;
    int32_t fl, fr, rl, rr, maxabs;

    /* ---- 标准麦轮逆解（四个轮子各算一次线性组合） ---- */
    fl = (int32_t)vx - v - (int32_t)omega;
    fr = (int32_t)vx + v + (int32_t)omega;
    rl = (int32_t)vx + v - (int32_t)omega;
    rr = (int32_t)vx - v + (int32_t)omega;

    /*
    三个自由度一起给满时，四轮里最大的那个会超量程。
    这时候把四个轮子按同一个比例一起缩小 —— 不能单独截断，
    否则前进 / 横移 / 自转之间的比例被改了，车会走歪。
    */
    maxabs = iabs32(fl);
    if (iabs32(fr) > maxabs) maxabs = iabs32(fr);
    if (iabs32(rl) > maxabs) maxabs = iabs32(rl);
    if (iabs32(rr) > maxabs) maxabs = iabs32(rr);

    if (maxabs > CHASSIS_SPEED_MAX)
    {
        fl = fl * CHASSIS_SPEED_MAX / maxabs;
        fr = fr * CHASSIS_SPEED_MAX / maxabs;
        rl = rl * CHASSIS_SPEED_MAX / maxabs;
        rr = rr * CHASSIS_SPEED_MAX / maxabs;
    }

    /* ---- 写硬件 ---- */
    motor_set(MOTOR_FL, (int16_t)fl);
    motor_set(MOTOR_FR, (int16_t)fr);
    motor_set(MOTOR_RL, (int16_t)rl);
    motor_set(MOTOR_RR, (int16_t)rr);
}

void chassis_stop(void)
{
    motor_stop_all();
}

void chassis_forward(int16_t speed)        { chassis_drive(speed, 0, 0); }
void chassis_backward(int16_t speed)       { chassis_drive((int16_t)(-speed), 0, 0); }
void chassis_turn_left(int16_t speed)      { chassis_drive(0, 0, speed); }
void chassis_turn_right(int16_t speed)     { chassis_drive(0, 0, (int16_t)(-speed)); }
void chassis_strafe_left(int16_t speed)    { chassis_drive(0, (int16_t)(-speed), 0); }
void chassis_strafe_right(int16_t speed)   { chassis_drive(0, speed, 0); }

/* 单轮测试（上电自检）：只动指定的那个轮子，其余三轮停住 */
void chassis_wheel(uint8_t wheel, int16_t speed)
{
    if (wheel < MOTOR_FL || wheel > MOTOR_RR) return;
    if (speed >  CHASSIS_SPEED_MAX) speed =  CHASSIS_SPEED_MAX;
    if (speed < -CHASSIS_SPEED_MAX) speed = -CHASSIS_SPEED_MAX;

    for (uint8_t w = MOTOR_FL; w <= MOTOR_RR; w++)
        motor_set(w, (w == wheel) ? speed : 0);
}
