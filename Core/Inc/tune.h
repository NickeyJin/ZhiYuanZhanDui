/*
tune.h —— 现场调参（麦轮底盘用的那一套）

这个模块只干两件事，都是"调车的时候才用、平时开车不用管"：
    ① 一个现场旋钮（按住 SELECT 再按十字键，值复位不丢、掉电才丢）：
       SELECT + ↑ / ↓              前后配平：调"前一对轮子 vs 后一对轮子"的配速差
       SELECT + ← / →              总速度（四个轮子一起）
       SELECT + L2/L1/R2/R1 + ← / → 单轮补偿：FL / RL / FR / RR
    ② 灯语：按键快闪 / 摇杆中闪 / 常亮 / 急停慢闪

按键表（完整版，开车时一般不用碰这些）：
    SELECT + ↑ / ↓   配平旋钮，↑ = 前一对轮子相对变快。一格 5，按住 L1 一格 20，
                     范围 ±300，每 250ms 走一格，复位不丢、掉电才丢。
                     方向：左移时车头顺时针转 → 按 ↑；逆时针转 → 按 ↓。
    SELECT + ← / →   速度旋钮，一格 2%（20‰），总速度 100~1250‰、单轮 ±600‰。

为什么不做串口输出：这块板子只接了 SWD，没有 USB-TTL。
读数据的命令（读的时候芯片会短暂 halt，但 .persist 段不受影响）：
    powershell -NoProfile -ExecutionPolicy Bypass -File build\tools\read_vars.ps1 -Elf <elf> -Symbols 't_speed_scale,t_wheel_delta,t_trim_delta'
*/
#ifndef TUNE_H
#define TUNE_H

#include <stdint.h>

void tune_init(void);            /* 上电：闪 3 下 + 恢复上次调好的值 */
void tune_update(void);          /* 每 20ms：处理两个现场旋钮 */
void tune_led(void);             /* 灯语：按键快闪 / 摇杆中闪 / 常亮 */
void tune_led_slow(void);        /* 急停慢闪 */

#endif /* TUNE_H */
