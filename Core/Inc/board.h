/*
board.h —— 板级零碎：LED 灯态 + 按键/摇杆的公共小工具
    PC13 是低电平点亮（共阳接法），所以 LED_ON() 是 RESET，别被名字骗了。

    灯态都写成 inline 小函数，原因是：它们只在最内层被调用一次，
    单独放个 .c 反而要多一次函数调用开销；而且这些"闪法"散落在
    各个文件里抄一遍太啰嗦了，谁看都得皱眉。
*/
#ifndef BOARD_H
#define BOARD_H

#include "main.h"
#include <stdint.h>

#define LED_ON()    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET)
#define LED_OFF()   HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET)

/*
按时间取模翻灯：xxms 一到就翻一下然后立刻返回。
绝对不能用 HAL_Delay() —— 20ms 的控制节拍被它一顶就爆，手柄直接掉帧（血的教训）。
*/
static inline void led_blink_ms(uint32_t period_ms)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13,
        ((HAL_GetTick() / period_ms) & 1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static inline void led_fast(void) { led_blink_ms(80);  }   /* 一键流程正在跑 */
static inline void led_mid(void)  { led_blink_ms(200); }   /* 摇杆推离了中位 */
static inline void led_slow(void) { led_blink_ms(300); }   /* 急停中 */

#endif /* BOARD_H */
