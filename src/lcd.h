// ST7789 320x240 on the 16-bit parallel bus, plus a small bitmap font.
#pragma once
#include <stdint.h>

#define LCD_W 320
#define LCD_H 240

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

// Palette (same as the PC page mock-up: colour = meaning)
#define C_BG     RGB(14, 18, 21)
#define C_BAR    RGB(22, 28, 33)
#define C_GRID   RGB(31, 40, 47)
#define C_GRID2  RGB(52, 65, 76)
#define C_TEXT   RGB(241, 239, 232)
#define C_MUTED  RGB(136, 135, 128)
#define C_WAVE   RGB(93, 202, 165)      // signal (teal)
#define C_WAVED  RGB(15, 110, 86)
#define C_TIME   RGB(133, 183, 235)     // time base (blue)
#define C_TRIG   RGB(239, 159, 39)      // trigger (amber)
#define C_RED    RGB(226, 75, 74)
#define C_BOX    RGB(38, 48, 58)

// small icons (about 12 px tall), drawn at x, y = top-left
void icon_updown(int x, int y, uint16_t c, uint16_t bg);            // vertical position
void icon_edge(int x, int y, int rising, uint16_t c, uint16_t bg);  // trigger edge
void icon_square(int x, int y, uint16_t c, uint16_t bg);            // generator
void icon_cross(int x, int y, uint16_t c);                          // red X: switched off
void icon_pc(int x, int y, int state, uint16_t bg);                 // 0 grey, 1 yellow broken, 2 green
void icon_battery(int x, int y, int level, int charging, uint16_t bg); // level 0..4, -1 unknown
void icon_arrow(int x, int y, int up, uint16_t c);                  // filled triangle, 9 px wide

void lcd_init(void);
void lcd_fill(int x, int y, int w, int h, uint16_t c);
void lcd_pixel(int x, int y, uint16_t c);
int  lcd_char(int x, int y, char ch, int s, uint16_t fg, uint16_t bg);
int  lcd_text(int x, int y, const char* t, int s, uint16_t fg, uint16_t bg);
int  lcd_text_w(const char* t, int s);
