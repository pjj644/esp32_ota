/*
 * SSD1306 OLED 128x64 驱动 (I2C) —— HAL 实现
 *
 * 接线: I2C1 -> PB6(SCL) / PB7(SDA), 地址 0x3C (SA0=GND) 或 0x3D (SA0=3V3)。
 * 帧缓冲布局: buffer[page*128 + col], 每字节 bit0 = 该页顶行 (与 SSD1306 RAM 一致)。
 */
#include "ssd1306.h"
#include "fonts.h"
#include <string.h>

#define SSD1306_I2C_TIMEOUT  100   /* ms */
#define SSD1306_I2C_RETRIES  2     /* 忙/超时重试次数 */

/* 注意: F1 HAL 的 DevAddress 必须传"左移一位的 8 位地址"(0x78), 不是 7 位 0x3C。
 * 误传 0x3C 会让硬件 I2C 永远 NAK (曾被误判为"GD32 硬件 I2C 坏", 引出过 bit-bang 兜底)。 */

static void ssd1306_write_cmd(SSD1306_t *dev, uint8_t cmd)
{
    /* 与参考工程 LED3 一致: 用 HAL_I2C_Master_Transmit 发 [0x00, cmd] 一帧 */
    uint8_t frame[2] = {0x00, cmd};
    for (int i = 0; i < SSD1306_I2C_RETRIES; i++) {
        if (HAL_I2C_Master_Transmit(dev->hi2c, (uint16_t)(dev->addr << 1), frame, 2,
                                    SSD1306_I2C_TIMEOUT) == HAL_OK) {
            return;
        }
        HAL_Delay(2);
    }
}

static void ssd1306_write_data(SSD1306_t *dev, uint8_t *data, uint16_t len)
{
    /* 与参考工程 LED3 一致: 一次 Master_Transmit 发 [0x40, data...] 整帧 */
    static uint8_t frame[SSD1306_WIDTH + 1];
    frame[0] = 0x40;
    memcpy(&frame[1], data, len);
    for (int i = 0; i < SSD1306_I2C_RETRIES; i++) {
        if (HAL_I2C_Master_Transmit(dev->hi2c, (uint16_t)(dev->addr << 1), frame,
                                    (uint16_t)(len + 1), SSD1306_I2C_TIMEOUT) == HAL_OK) {
            return;
        }
        HAL_Delay(2);
    }
}

uint8_t SSD1306_Probe(SSD1306_t *dev)
{
    /* 与 LED3 OLED_CheckConnection 一致: 发 [0x00, 0xAE] 一帧探测 (0xAE=关显示, 无害)。
     * F1 HAL 地址传 0x78/0x7A (= 0x3C/0x3D << 1)。 */
    uint8_t check[2] = {0x00, 0xAE};
    if (HAL_I2C_Master_Transmit(dev->hi2c, 0x78, check, 2, 100) == HAL_OK) {
        dev->addr = 0x3C;
        return 1;
    }
    if (HAL_I2C_Master_Transmit(dev->hi2c, 0x7A, check, 2, 100) == HAL_OK) {
        dev->addr = 0x3D;
        return 1;
    }
    return 0;
}

HAL_StatusTypeDef SSD1306_Init(SSD1306_t *dev)
{
    /* 与参考工程 LED3 的根目录 OLED.c (真正工作的驱动, D:/tools1/LED3/MDK-ARM/OLED.c)
     * 逐字节一致, 且与 OLED_Init 相同: 探测后延时 100ms, 只发命令不发空白帧。
     * (OLED_Init 初始化后是直接画内容 + ShowFrame, 不做单独的 Clear/空白整屏上传) */
    HAL_Delay(100);

    static const uint8_t init_seq[] = {
        0xAE,             /* 关显示 */
        0xA8, 0x3F,       /* 多路复用: 1/64 行 */
        0xD3, 0x00,       /* 显示偏移 0 */
        0x40,             /* 起始行 0 */
        0xA1,             /* 段重映射 */
        0xC8,             /* COM 扫描方向 */
        0xDA, 0x12,       /* COM 引脚配置: alternate, 64 行 */
        0x81, 0x7F,       /* 对比度 */
        0xA6,             /* 正常显示 (非反显) */
        0x8D, 0x14,       /* 电荷泵开启 */
        0xAF              /* 开显示 */
    };

    for (uint32_t i = 0; i < sizeof(init_seq); i++) {
        ssd1306_write_cmd(dev, init_seq[i]);
    }
    /* 与 LED3 OLED_Init 一致: 初始化后上传一帧空白 (清 GDDRAM) */
    SSD1306_Clear(dev);
    return SSD1306_Update(dev);
}

void SSD1306_Clear(SSD1306_t *dev)
{
    for (uint32_t i = 0; i < sizeof(dev->buffer); i++) {
        dev->buffer[i] = 0x00;
    }
}

void SSD1306_FillRect(SSD1306_t *dev, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t on)
{
    for (uint8_t yy = 0; yy < h; yy++) {
        for (uint8_t xx = 0; xx < w; xx++) {
            SSD1306_DrawPixel(dev, (uint8_t)(x + xx), (uint8_t)(y + yy), on);
        }
    }
}

void SSD1306_DrawPixel(SSD1306_t *dev, uint8_t x, uint8_t y, uint8_t on)
{
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
        return;
    }
    uint8_t *p = &dev->buffer[(y / 8) * SSD1306_WIDTH + x];
    if (on) {
        *p |= (uint8_t)(1u << (y % 8));
    } else {
        *p &= (uint8_t)~(1u << (y % 8));
    }
}

HAL_StatusTypeDef SSD1306_Update(SSD1306_t *dev)
{
    /* 8 页整屏上传 (与 LED3 一致, 64 行面板) */
    uint8_t pages = SSD1306_HEIGHT / 8;
    for (uint8_t page = 0; page < pages; page++) {
        ssd1306_write_cmd(dev, (uint8_t)(0xB0 + page));
        ssd1306_write_cmd(dev, 0x00);   /* 列低地址 */
        ssd1306_write_cmd(dev, 0x10);   /* 列高地址 */
        ssd1306_write_data(dev, &dev->buffer[page * SSD1306_WIDTH], SSD1306_WIDTH);
    }
    return HAL_OK;
}

void SSD1306_DrawChar8x16(SSD1306_t *dev, uint8_t x, uint8_t y, char ch)
{
    if (ch < 0x20 || ch > 0x7E) {
        ch = '?';
    }
    /* font_ascii_8x16 (LED3 OLED_F8x16 布局): 每字符 16 字节, 前 8 字节=8 列上半(行0-7),
     * 后 8 字节=8 列下半(行8-15), bit0 = 顶行。 */
    const uint8_t *glyph = font_ascii_8x16[ch - 0x20];
    for (uint8_t col = 0; col < 8; col++) {
        uint8_t b0 = glyph[col];
        uint8_t b1 = glyph[col + 8];
        for (uint8_t row = 0; row < 8; row++) {
            if (b0 & (1u << row)) {
                SSD1306_DrawPixel(dev, (uint8_t)(x + col), (uint8_t)(y + row), 1);
            }
            if (b1 & (1u << row)) {
                SSD1306_DrawPixel(dev, (uint8_t)(x + col), (uint8_t)(y + row + 8), 1);
            }
        }
    }
}

void SSD1306_DrawString8x16(SSD1306_t *dev, uint8_t x, uint8_t y, const char *str)
{
    while (*str) {
        if (x + 8 > SSD1306_WIDTH) {
            break;
        }
        SSD1306_DrawChar8x16(dev, x, y, *str);
        x = (uint8_t)(x + 8);
        str++;
    }
}

void SSD1306_DrawHZ16(SSD1306_t *dev, uint8_t x, uint8_t y, uint8_t idx)
{
    if (idx >= FONT_HZ16_COUNT) {
        return;
    }
    const uint8_t *glyph = font_hz16_data[idx];
    for (uint8_t col = 0; col < 16; col++) {
        uint8_t b0 = glyph[col * 2];
        uint8_t b1 = glyph[col * 2 + 1];
        for (uint8_t row = 0; row < 8; row++) {
            if (b0 & (1u << row)) {
                SSD1306_DrawPixel(dev, (uint8_t)(x + col), (uint8_t)(y + row), 1);
            }
            if (b1 & (1u << row)) {
                SSD1306_DrawPixel(dev, (uint8_t)(x + col), (uint8_t)(y + row + 8), 1);
            }
        }
    }
}

void SSD1306_DrawString16(SSD1306_t *dev, uint8_t x, uint8_t y, const char *str)
{
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            if (x + 8 > SSD1306_WIDTH) {
                break;
            }
            SSD1306_DrawChar8x16(dev, x, y, (char)c);
            x = (uint8_t)(x + 8);
            str++;
        } else if ((c & 0xF0) == 0xE0 && str[1] && str[2]) {
            /* 3 字节 UTF-8 -> UCS-2 */
            uint16_t cp = (uint16_t)(((uint16_t)(c & 0x0F) << 12) |
                                     ((uint16_t)((uint8_t)str[1] & 0x3F) << 6) |
                                     ((uint8_t)str[2] & 0x3F));
            int idx = (cp >= 0x4E00) ? font_hz16_find(cp) : -1;
            if (idx >= 0) {
                SSD1306_DrawHZ16(dev, x, y, (uint8_t)idx);
                x = (uint8_t)(x + 16);
            } else {
                x = (uint8_t)(x + 16);
            }
            str += 3;
        } else {
            x = (uint8_t)(x + 8);
            str++;
        }
    }
}
