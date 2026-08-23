#ifndef FONTS_H
#define FONTS_H

#include <stdint.h>

/* 95 个可打印 ASCII (0x20..0x7E), 每字符 6 字节 (6x8, 列优先, bit0=顶) */
extern const uint8_t font_ascii_6x8[95][6];

/* 95 个可打印 ASCII (0x20..0x7E), 每字符 16 字节 (8x16, 列优先, bit0=顶) */
extern const uint8_t font_ascii_8x16[95][16];

/* 内置中文 (宋体 16x16), 按 Unicode 码点查表 */
#define FONT_HZ16_COUNT 4

extern const uint16_t font_hz16_index[FONT_HZ16_COUNT];
extern const uint8_t font_hz16_data[FONT_HZ16_COUNT][32];

/* 返回码点在索引表中的位置, 未收录返回 -1 */
int font_hz16_find(uint16_t ucs2);

#endif /* FONTS_H */
