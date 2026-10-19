/*
chassis.h —— 底盘运动层（麦轮全向解算，标准逆运动学）

麦轮有三个自由度，可以同时叠加，所以它能"想去哪就去哪"：
    vx     前进 / 后退
    vy     左 / 右横移
    omega  原地自转

========================= 标准逆运动学 =========================
四轮转速 = 三个自由度的线性组合（每轮 2 路 PWM，转向由正负号决定）：

    FL = vx - vy - omega
    FR = vx + vy + omega
    RL = vx + vy - omega
    RR = vx - vy + omega

教材/论文里的标准形式是：

    [w_FL]   [ 1 -1 -(Lx+Ly)] [vx   ]
    [w_FR] = [ 1 +1 +(Lx+Ly)] [vy   ] / R
    [w_RL]   [ 1 +1 -(Lx+Ly)] [omega]
    [w_RR]   [ 1 -1 +(Lx+Ly)]

Lx / Ly 是轮心到车体中心的半长 / 半宽，R 是轮半径。
这里把 (Lx+Ly)/R 直接折进 omega 的量纲 —— 也就是 omega 用"轮子速度"当单位，
不做那次乘除。轮子对称布置时四轮的 (Lx+Ly) 完全一样，所以不影响正确性，
但少了一次浮点乘法（F103 没有 FPU，整数解算更快）。

检查一下符号对不对（纯横移，vy = -V，其余为 0）：
    FL = +V, FR = -V, RL = -V, RR = +V
左前、右后往前转，右前、左后往后转 —— 四轮速度矢量合成后整车向左平移，
这就是麦轮横移的标准形态。

坐标约定（人站在车后面往前看）：
    vx > 0  前进        vx < 0  后退
    vy > 0  向右平移    vy < 0  向左平移
    omega > 0  逆时针   omega < 0  顺时针

用法：
    chassis_init();                          // 初始化电机 PWM
    chassis_drive(vx, vy, omega);            // 三个都是 -800 ~ +800
    chassis_stop();                          // 停车
================================================================
*/
#ifndef CHASSIS_H
#define CHASSIS_H

#include <stdint.h>

/* 底盘速度上限。比 MOTOR_SPEED_MAX 小，留出余量给"三自由度叠加"时的等比缩放 */
#define CHASSIS_SPEED_MAX   800

/* 横移方向开关。实测发现"右推摇杆车往左走"就把这里改成 (-1) */
#define CHASSIS_STRAFE_SIGN   (+1)

void chassis_init(void);                    /* 初始化电机 PWM */
void chassis_stop(void);                    /* 四轮滑行停车 */

/* 全向指令：前进 + 横移 + 自转，三个分量同时生效 */
void chassis_drive(int16_t vx, int16_t vy, int16_t omega);

/* 下面几个是单自由度接口，给自检 / 调参测试用 */
void chassis_forward(int16_t speed);
void chassis_backward(int16_t speed);
void chassis_turn_left(int16_t speed);
void chassis_turn_right(int16_t speed);
void chassis_strafe_left(int16_t speed);
void chassis_strafe_right(int16_t speed);

/* 单独转某一个轮子（自检用），wheel = MOTOR_FL ~ MOTOR_RR */
void chassis_wheel(uint8_t wheel, int16_t speed);

#endif /* CHASSIS_H */
