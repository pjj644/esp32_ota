/*
 * SSD1306 OLED 128x64 驱动 (I2C) —— HAL 实现
 *
 * 接线: I2C1 -> PB6(SCL) / PB7(SDA), 地址 0x3C (SA0=GND) 或 0x3D (SA0=3V3)。
 * 帧缓冲布局: buffer[page*128 + col], 每字节 bit0 = 该页顶行 (与 SSD1306 RAM 一致)。
 */
#include "ssd1306.h"
#include "fonts.h"

#define SSD1306_I2C_TIMEOUT  100   /* ms */
#define SSD1306_I2C_RETRIES  2     /* 忙/超时重试次数 */

/* 通信方式: 0 = 硬件 I2C (正常 STM32), 1 = bit-bang 软件 I2C。
 * 实测该板 (GD32 克隆 F103) 硬件 I2C1 外设对 OLED 无 ACK, 自动降级 bit-bang。 */
static uint8_t ssd1306_use_bitbang = 0;

uint8_t SSD1306_IsBitBang(const SSD1306_t *dev)
{
    (void)dev;
    return ssd1306_use_bitbang;
}

/* bit-bang 层 (定义在本文件后半部) */
static void ssd1306_bb_gpio_init(void);
static uint8_t bb_probe_addr(uint8_t addr7);
static void bb_tx_frame(SSD1306_t *dev, uint8_t ctrl, const uint8_t *data, uint16_t len);

static void ssd1306_write_cmd(SSD1306_t *dev, uint8_t cmd)
{
    if (ssd1306_use_bitbang) {
        bb_tx_frame(dev, 0x00, &cmd, 1);
        return;
    }
    for (int i = 0; i < SSD1306_I2C_RETRIES; i++) {
        if (HAL_I2C_Mem_Write(dev->hi2c, dev->addr, 0x00,
                              I2C_MEMADD_SIZE_8BIT, &cmd, 1,
                              SSD1306_I2C_TIMEOUT) == HAL_OK) {
            return;
        }
        HAL_Delay(2);
    }
    dev->i2c_errs++;
}

static void ssd1306_write_data(SSD1306_t *dev, uint8_t *data, uint16_t len)
{
    if (ssd1306_use_bitbang) {
        bb_tx_frame(dev, 0x40, data, len);
        return;
    }
    for (int i = 0; i < SSD1306_I2C_RETRIES; i++) {
        if (HAL_I2C_Mem_Write(dev->hi2c, dev->addr, 0x40,
                              I2C_MEMADD_SIZE_8BIT, data, len,
                              SSD1306_I2C_TIMEOUT) == HAL_OK) {
            return;
        }
        HAL_Delay(2);
    }
    dev->i2c_errs++;
}

uint8_t SSD1306_Probe(SSD1306_t *dev)
{
    /* 1) 先试硬件 I2C (标准 STM32 的正常路径) */
    if (HAL_I2C_IsDeviceReady(dev->hi2c, 0x3C, 2, 100) == HAL_OK) {
        dev->addr = 0x3C;
        return 1;
    }
    if (HAL_I2C_IsDeviceReady(dev->hi2c, 0x3D, 2, 100) == HAL_OK) {
        dev->addr = 0x3D;
        return 1;
    }

    /* 2) 硬件不行 -> 切 bit-bang 软件 I2C 再试 (GD32 克隆外设异常时有用) */
    ssd1306_use_bitbang = 1;
    ssd1306_bb_gpio_init();
    if (bb_probe_addr(0x3C)) {
        dev->addr = 0x3C;
        return 1;
    }
    if (bb_probe_addr(0x3D)) {
        dev->addr = 0x3D;
        return 1;
    }
    ssd1306_use_bitbang = 0;
    return 0;
}

/* ---- 软件 I2C (bit-bang) 传输层: 硬件 I2C 外设异常时替代 ---- */
#define BB_SCL_PIN   GPIO_PIN_6
#define BB_SDA_PIN   GPIO_PIN_7

static void bb_delay(void)
{
    volatile uint32_t n = 120;   /* ~2-3us @72MHz */
    while (n--) {
        __NOP();
    }
}

static void bb_scl_hi(void) { GPIOB->BSRR = BB_SCL_PIN; }
static void bb_scl_lo(void) { GPIOB->BRR  = BB_SCL_PIN; }
static void bb_sda_hi(void) { GPIOB->BSRR = BB_SDA_PIN; }
static void bb_sda_lo(void) { GPIOB->BRR  = BB_SDA_PIN; }

static uint8_t bb_sda_read(void)
{
    return (GPIOB->IDR & BB_SDA_PIN) ? 1 : 0;
}

static void bb_start(void)
{
    bb_sda_hi(); bb_scl_hi(); bb_delay();
    bb_sda_lo(); bb_delay();
    bb_scl_lo(); bb_delay();
}

static void bb_stop(void)
{
    bb_sda_lo(); bb_delay();
    bb_scl_hi(); bb_delay();
    bb_sda_hi(); bb_delay();
}

/* 发 1 字节 (MSB 先), 返回 1 = 从机 ACK */
static uint8_t bb_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) { bb_sda_hi(); } else { bb_sda_lo(); }
        b <<= 1;
        bb_scl_hi(); bb_delay();
        bb_scl_lo(); bb_delay();
    }
    bb_sda_hi();               /* 释放 SDA 等 ACK */
    bb_delay();
    bb_scl_hi(); bb_delay();
    uint8_t ack = bb_sda_read() ? 0 : 1;
    bb_scl_lo(); bb_delay();
    return ack;
}

/* 复位 I2C1 外设, 把 PB6/PB7 切成通用开漏输出 */
static void ssd1306_bb_gpio_init(void)
{
    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();
    GPIO_InitTypeDef g = {0};
    g.Pin = BB_SCL_PIN | BB_SDA_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &g);
    bb_scl_hi(); bb_sda_hi(); bb_delay();
}

/* 探测单地址: START + 地址字节, 有 ACK 即命中 */
static uint8_t bb_probe_addr(uint8_t addr7)
{
    bb_start();
    uint8_t ack = bb_write_byte((uint8_t)(addr7 << 1));
    bb_stop();
    return ack;
}

/* 传输一帧: [addr|W][ctrl][data...] (控制字节 0x00=命令 / 0x40=数据) */
static void bb_tx_frame(SSD1306_t *dev, uint8_t ctrl, const uint8_t *data, uint16_t len)
{
    bb_start();
    if (!bb_write_byte((uint8_t)(dev->addr << 1))) {
        dev->i2c_errs++;
        bb_stop();
        return;
    }
    if (!bb_write_byte(ctrl)) {
        dev->i2c_errs++;
        bb_stop();
        return;
    }
    for (uint16_t i = 0; i < len; i++) {
        if (!bb_write_byte(data[i])) {
            dev->i2c_errs++;
            break;
        }
    }
    bb_stop();
}

/* 全地址扫描 (诊断用): 返回 ACK 的地址数, found 回填前 max_found 个 */
uint8_t SSD1306_BusScanBitBang(uint8_t *found, uint8_t max_found)
{
    ssd1306_bb_gpio_init();
    uint8_t n = 0;
    for (uint16_t addr = 0x01; addr < 0x80; addr++) {
        if (bb_probe_addr((uint8_t)addr)) {
            if (n < max_found) {
                found[n] = (uint8_t)addr;
            }
            n++;
        }
    }
    return n;
}

HAL_StatusTypeDef SSD1306_Init(SSD1306_t *dev)
{
    static const uint8_t init_seq[] = {
        0xAE,             /* 关显示 */
        0xD5, 0x80,       /* 时钟分频 */
        0xA8, 0x1F,       /* 多路复用: 1/32 行 (实测克隆屏 64 行配置会导致每行重复/叠影) */
        0xD3, 0x00,       /* 显示偏移 0 */
        0x40,             /* 起始行 0 */
        0x8D, 0x14,       /* 电荷泵开启 */
        0x20, 0x02,       /* 页寻址模式 */
        0xA1,             /* 段重映射 (正常方向) */
        0xC8,             /* COM 扫描方向 */
        0xDA, 0x12,       /* COM 引脚配置 (0x02 实测更花) */
        0x81, 0xCF,       /* 对比度 */
        0xD9, 0xF1,       /* 预充电周期 */
        0xDB, 0x40,       /* VCOMH 取消选择电平 */
        0xA4,             /* 从 RAM 恢复显示 */
        0xA6,             /* 正常显示 (非反显) */
        0xAF              /* 开显示 */
    };

    for (uint32_t i = 0; i < sizeof(init_seq); i++) {
        ssd1306_write_cmd(dev, init_seq[i]);
    }
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
    /* 实测克隆屏只有 4 页 RAM (32 行): 写页 4-7 会别名校回页 0-3, 造成内容重复。
     * 因此只上传前 4 页。 */
    uint8_t pages = 4;
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
    const uint8_t *glyph = font_ascii_8x16[ch - 0x20];
    for (uint8_t col = 0; col < 8; col++) {
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
