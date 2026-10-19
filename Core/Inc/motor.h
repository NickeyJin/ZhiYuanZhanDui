/*
motor.h —— 单轮电机硬件层（DRV8833 双 PWM 驱动）

一片 DRV8833 带两个 H 桥，每个 H 桥用 IN1 / IN2 两根脚：
    IN1=0    IN2=0     高阻滑行（停）
    IN1=PWM  IN2=0     正转，占空比 = 转速
    IN1=0    IN2=PWM   反转，占空比 = 转速
    IN1=PWM  IN2=PWM   刹车（上下桥同时导通）

一个轮子吃 2 路 PWM，四个轮子 8 路：
    1 左前 FL   PA0 / PA1    TIM2_CH1 / CH2
    2 右前 FR   PA2 / PA3    TIM2_CH3 / CH4
    3 左后 RL   PB0 / PB1    TIM3_CH3 / CH4
    4 右后 RR   PA8 / PA11   TIM1_CH1 / CH4

用法：motor_init();  motor_set(轮号, -1000 ~ +1000);
    speed > 0 = 该轮按"车前进"的方向转（接反的轮子在 motor_map 里翻 invert）

===================== PWM 时基：20kHz，满速 40% =====================
72MHz / (PSC+1) / (ARR+1) = 72e6 / 72 / 50 = 20000 Hz
    (ARR+1) = 50 个计数一个周期，一个计数 = 1µs
    满速（speed = 1000）对应 CCR = 50 × 40% = 20

为什么是 20kHz：超出人耳可听范围，电机不啸叫；也避开了低频 PWM 的抖动。
为什么满速只给 40%：轮子现在接在 8V 档上，40% ≈ 3.2V 等效，是这颗 TT 马达
在"跑得动又不打滑、不发热"之间的标准位。再往上只会更快地磨轮子。
    ★ 如果实测发现满速也不够劲，只改 MOTOR_DUTY_PERCENT 这一个数（40 → 50 或 60），
      chassis.c / app.c 里什么都不用动。
*/
#ifndef MOTOR_H
#define MOTOR_H

#include <stdint.h>

/* 速度满量程：motor_set() 的取值范围就是 -MOTOR_SPEED_MAX ~ +MOTOR_SPEED_MAX */
#define MOTOR_SPEED_MAX     1000

/* PWM 时基（对应 main.c 里 TIM1/TIM2/TIM3 的 Prescaler / Period） */
#define MOTOR_PWM_PSC       71      /* 72MHz / 72 = 1MHz，一个计数 1µs */
#define MOTOR_PWM_ARR       49      /* (49+1) 个计数 = 50µs = 20kHz */

/* 一个 PWM 周期的计数值 */
#define MOTOR_PWM_COUNTS    (MOTOR_PWM_ARR + 1U)              /* 50 */

/* 满速对应的占空比（%），也是 CCR 的上限
   ★ 2026-10-19 临时从 40 提到 80 做测试：40% 时满速只有 32% 占空比
     （8V × 32% ≈ 2.56V 等效），实测四个轮子完全不动 —— 说明对现在这台
     加了两节 18650 + 3D 打印盒 + 机械臂的车来说，2.56V 太小了。
     先给 80% 确认电机能转，再往回调到刚够用的值。 */
#define MOTOR_DUTY_PERCENT  80
#define MOTOR_PWM_CCR_MAX   (MOTOR_PWM_COUNTS * MOTOR_DUTY_PERCENT / 100U)   /* 20 */

/* 轮位编号（下标从 1 开始，和硬件丝印 FL/FR/RL/RR 一一对应） */
#define MOTOR_FL   1    /* 左前 */
#define MOTOR_FR   2    /* 右前 */
#define MOTOR_RL   3    /* 左后 */
#define MOTOR_RR   4    /* 右后 */

/* ---- 每轮微调（千分比）：补偿四个电机/轮子的个体差异 ----
    开环驱动时四个轮子拿到同一个 PWM，实际转速却可能差 20~30%，
    差得多时车会自己跑偏、横移变成画圈。这里给每一路单独按比例缩放：
        实际输出 = 指令 × 微调 / 1000
        1000 = 不调（默认）
        调小 = 这个轮子变慢
        调大 = 这个轮子变快

    ★ 两个旋钮各管一件事，分开调：
        横移画圈 / 自转时整车往侧面溜 → 只看"前一对 FL+FR"和"后一对 RL+RR"
            左移时车头顺时针转 = 前面偏慢 → 调大 FL/FR，或调小 RL/RR
            左移时车头逆时针转 = 前面偏快 → 调小 FL/FR，或调大 RL/RR
        前进/后退跑偏（车头歪）→ 只看"左一对 FL+RL"和"右一对 FR+RR"
            往左偏 = 右边快 → 调小 FR/RR 或调大 FL/RL
            往右偏 = 左边快 → 反过来
    一次只改一个数、一次改 50~100，跑一遍看轨迹。

    ★ 不想反复重烧：开车固件里按住 SELECT + 十字键 ↑/↓ 可以实时调"前后配平"
      （前一对 +d、后一对 -d，掉电保存），调到横移不转了再把值抄回这里。

    刚重构完（2026-10-19）四个值全部回到 1000 = 不补偿。
    这次换到 8V + 40% 占空比，之前的 840/1110 是旧供电下测的，已经作废。
*/
#ifndef MOTOR_TRIM_FL
#define MOTOR_TRIM_FL   1000
#endif
#ifndef MOTOR_TRIM_FR
#define MOTOR_TRIM_FR   1000
#endif
#ifndef MOTOR_TRIM_RL
#define MOTOR_TRIM_RL   1000
#endif
#ifndef MOTOR_TRIM_RR
#define MOTOR_TRIM_RR   1000
#endif
#define MOTOR_TRIM_BASE 1000    /* 微调的基准，不用改 */

/* ---- 起步踹一脚（破静摩擦）----
   轮子从静止开始转、或者反向的那一瞬间，静摩擦最大，低占空比下电机会"嗡嗡半天不动"。
   这里在起步后的头 MOTOR_KICK_MS 毫秒把输出临时放大 MOTOR_KICK_GAIN%，越过静摩擦后
   自动掉回正常值 —— 反映到波形上就是"开头一段高电平，之后转平稳"。

       0        = 关掉（不需要踹的时候把 MOTOR_KICK_MS 改成 0）
       90ms     = 默认，够把轮子推起来又不会明显冲出去
       200      = 放大到 2 倍（最终仍钳在 MOTOR_SPEED_MAX = 满 80% 占空比）
       450      = 指令小于这个值就不踹（轻轻推摇杆时不该一顿一顿的）
*/
#ifndef MOTOR_KICK_MS
#define MOTOR_KICK_MS       90
#endif
#ifndef MOTOR_KICK_GAIN
#define MOTOR_KICK_GAIN     200
#endif
#ifndef MOTOR_KICK_MIN
#define MOTOR_KICK_MIN      450
#endif

void motor_init(void);                          /* 启动 8 路 PWM 并让四轮停住 */
void motor_set(uint8_t wheel, int16_t speed);   /* 轮号 1~4，速度 -1000 ~ +1000 */
void motor_stop_all(void);                      /* 四轮高阻滑行 */
void motor_brake_all(void);                     /* 四轮刹车（急停） */

/* 调试用配平：motor_set_trim_delta(d) 让前一对 FL/FR 配速 +d、后一对 RL/RR 配速 -d。
   专门用来治"横移画圈 / 自转侧滑"，d=0 就是恢复 motor.h 里的值。
   d>0 = 前面相对变快，d<0 = 前面相对变慢。范围 ±300。 */
void    motor_set_trim_delta(int16_t delta);
int16_t motor_get_trim_delta(void);

/* ---- 运行期两个"软旋钮"（开车固件里用 SELECT + 按键实时调，掉电由 tune.c 保存）----

   ① 总速度倍率 speed_scale：所有轮子同时乘一个千分比
        1000 = 原样；800 = 整体打八折；1250 = 最多放大 25%
        它先乘（给后面的单轮补偿留出余量），再乘单轮微调
   ② 单轮附加偏移 wheel_delta：只给某一个轮子加/减千分比，范围 ±600
        专门补"某个轮子明显比别人慢"（比如左后轮机械卡滞）

   最终输出 = 指令 × speed_scale/1000 × (trim + trim_delta ± + wheel_delta)/1000
   例：总速度 800‰、左后轮 +400 → 其他轮 800，左后轮 800×1.4 = 1120 → 钳到 1000
*/
void    motor_set_speed_scale(int16_t scale);            /* 100 ~ 1250 */
int16_t motor_get_speed_scale(void);
void    motor_set_wheel_delta(uint8_t wheel, int16_t delta);   /* ±600，0 = 不补 */
int16_t motor_get_wheel_delta(uint8_t wheel);

#endif /* MOTOR_H */
