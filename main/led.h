#ifndef LED_H
#define LED_H

#include "driver/gpio.h"

#define LED_GPIO        GPIO_NUM_2      /* WROOM-32 开发板板载 LED */
#define BLINK_PERIOD_MS 1000            /* 闪烁周期，单位 ms */

/**
 * 初始化 LED 引脚（复位 + 设置为输出）。
 * app_main 启动时调用一次即可。
 */
void led_init(void);

/**
 * 翻转 LED 一次，并睡半个周期。
 * 返回翻转后的电平（0 或 1），方便上层打印心跳。
 */
int led_toggle(void);

#endif /* LED_H */
