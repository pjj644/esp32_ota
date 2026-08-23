/*
 * SSD1306 OLED 128x64 驱动 (I2C, 4-wire 之外最简: 2 线 SCL/SDA)
 *
 * 适配 STM32F103 HAL (I2C1 = PB6/SCL, PB7/SDA, 100kHz)。
 * 采用页寻址: 内部 1024B 帧缓冲 + 整屏上传。
 *
 * 字体:
 *   - ASCII 8x16 (公有领域 IBM VGA 8x8 纵向加倍, fonts.c)
 *   - 中文 16x16 (宋体子集, fonts.c)
 * 中英文混排用 SSD1306_DrawString16() (UTF-8 输入, 汉字 16px 宽, 其余 8px)。
 */
#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64

typedef struct {
    I2C_HandleTypeDef *hi2c;
    uint8_t  addr;                 /* 探测到的从机地址 (0x3C / 0x3D, 7 位) */
    uint8_t  present;              /* 1 = 探测成功, 0 = 无 OLED */
    uint8_t  buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];
} SSD1306_t;

/* 探测 0x3C/0x3D, 找到即记录 addr (7 位) 并返回 1。硬件 I2C 即可。 */
uint8_t SSD1306_Probe(SSD1306_t *dev);

/* 初始化并开显示, 返回 HAL_OK/HAL_ERROR */
HAL_StatusTypeDef SSD1306_Init(SSD1306_t *dev);

/* 帧缓冲操作 (仅改 RAM, 需再调 SSD1306_Update 上屏) */
void SSD1306_Clear(SSD1306_t *dev);
void SSD1306_FillRect(SSD1306_t *dev, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t on);
void SSD1306_DrawPixel(SSD1306_t *dev, uint8_t x, uint8_t y, uint8_t on);

/* 整屏上传 */
HAL_StatusTypeDef SSD1306_Update(SSD1306_t *dev);

/* 6x8 半角字符 (6px 宽, 8px 高, 128x64 屏幕每行 21 字符, 全屏可放 8 行) */
void SSD1306_DrawChar6x8(SSD1306_t *dev, uint8_t x, uint8_t y, char ch);
void SSD1306_DrawString6x8(SSD1306_t *dev, uint8_t x, uint8_t y, const char *str);

/* 8x16 半角字符 (8px 宽, 16px 高, 128x64 屏幕每行 16 字符, 全屏 4 行) */
void SSD1306_DrawChar8x16(SSD1306_t *dev, uint8_t x, uint8_t y, char ch);
void SSD1306_DrawString8x16(SSD1306_t *dev, uint8_t x, uint8_t y, const char *str);

/* 16x16 中文 (fonts.h 收录的字, 未收录显示空白) */
void SSD1306_DrawHZ16(SSD1306_t *dev, uint8_t x, uint8_t y, uint8_t idx);

/* UTF-8 混排: 汉字 16px 宽, 其余 8px */
void SSD1306_DrawString16(SSD1306_t *dev, uint8_t x, uint8_t y, const char *str);

#endif /* SSD1306_H */
