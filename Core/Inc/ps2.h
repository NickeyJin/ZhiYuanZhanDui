/*
 * ps2.h —— PS2 无线手柄输入层
 *
 * 硬件（接收器 6 针，供电给 3V3 最安全）：
 *     3V3  → 3V3
 *     GND  → GND（必须和主板共地）
 *     DATA → PA4（输入上拉，接收器 → MCU）
 *     CMD  → PA5（输出，MCU → 接收器）
 *     ATT  → PA6（输出，通信期间拉低，低有效）
 *     CLK  → PA7（输出）
 *
 * 用法： ps2_init();  然后主循环里 ps2_poll();
 *        ps2_poll() 之后看 ps2_ok 判断这一帧数据能不能用。
 */
#ifndef PS2_H
#define PS2_H

#include <stdint.h>

extern uint8_t ps2_ok;              /* 1 = 这一帧数据有效；0 = 掉线/没插 */
extern uint8_t ps2_btn1;            /* 按键组1（按下 = 1）*/
extern uint8_t ps2_btn2;            /* 按键组2（按下 = 1）*/
extern uint8_t ps2_lx, ps2_ly;      /* 左摇杆 左右 / 上下，中位 0x80 */
extern uint8_t ps2_rx, ps2_ry;      /* 右摇杆 左右 / 上下，中位 0x80 */

/*
 * 诊断用（调试器直接读变量就能看出问题在哪一层）：
 *   ps2_raw     最近一帧读回来的 9 个字节，不管合不合法都留着。
 *               没接接收器/没对码时 DATA 被内部上拉拉高，这 9 个字节就是全 0xFF。
 *   ps2_raw[1]  就是手柄回的 ID：0x41 = 数字模式（摇杆无效！）
 *                              0x73 / 0x53 = 模拟模式（摇杆有效）
 *   ps2_ok_cnt / ps2_bad_cnt  累计有效 / 无效帧数，看链路稳不稳。
 */
extern uint8_t  ps2_raw[9];
extern uint32_t ps2_ok_cnt;
extern uint32_t ps2_bad_cnt;
extern uint32_t ps2_mode_try_cnt;  /* 读到 0x41 时自动切模拟模式的重试次数 */
extern uint32_t ps2_mode_ok_cnt;   /* 其中成功的次数（>0 = 数字模式能自动救回来） */

/* PS2 摇杆的中位和量程 */
#define PS2_STICK_CENTER   0x80
#define PS2_STICK_MAX      127

/* ---- 按键组1 的 8 个位 ---- */
#define PS2_BTN_SELECT  0x01
#define PS2_BTN_R3      0x04
#define PS2_BTN_START   0x08
#define PS2_BTN_UP      0x10
#define PS2_BTN_RIGHT   0x20
#define PS2_BTN_DOWN    0x40
#define PS2_BTN_LEFT    0x80

/* ---- 按键组2 的 8 个位 ---- */
#define PS2_BTN_L2        0x01
#define PS2_BTN_R2        0x02
#define PS2_BTN_L1        0x04
#define PS2_BTN_R1        0x08
#define PS2_BTN_TRIANGLE  0x10   /* 三角 */
#define PS2_BTN_CIRCLE    0x20   /* 圆圈 */
#define PS2_BTN_CROSS     0x40   /* 叉叉 */
#define PS2_BTN_SQUARE    0x80   /* 方块 */

void ps2_init(void);
void ps2_poll(void);

#endif /* PS2_H */
