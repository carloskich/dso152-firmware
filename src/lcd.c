// ST7789 driver. Pins (checked on the unit): data PB0..15, CS PA15, RS PC13,
// WR PC14, RD PC15, RST PA8, backlight PC9. The panel needs INVON and the
// pixel data inverted to show the right colours.
#include "lcd.h"
#include "hw.h"
#include "font5x7.h"

#define RS_BIT (1u << 13)
#define WR_BIT (1u << 14)

static inline void wr_pulse(void) {
    GPIOC->BRR = WR_BIT;
    __NOP();
    GPIOC->BSRR = WR_BIT;
    __NOP();
}
static void cmd(uint8_t c) {
    GPIOC->BRR = RS_BIT;
    GPIOB->ODR = c;
    wr_pulse();
    GPIOC->BSRR = RS_BIT;
}
static inline void data(uint16_t d) {
    GPIOB->ODR = d;
    wr_pulse();
}
static void window(int x, int y, int w, int h) {
    int x1 = x + w - 1, y1 = y + h - 1;
    cmd(0x2A); data(x >> 8); data(x & 0xFF); data(x1 >> 8); data(x1 & 0xFF);
    cmd(0x2B); data(y >> 8); data(y & 0xFF); data(y1 >> 8); data(y1 & 0xFF);
    cmd(0x2C);
}

void lcd_fill(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    window(x, y, w, h);
    GPIOB->ODR = (uint16_t)~c;
    for (uint32_t n = (uint32_t)w * (uint32_t)h; n; n--) wr_pulse();
}

void lcd_pixel(int x, int y, uint16_t c) { lcd_fill(x, y, 1, 1, c); }

void lcd_init(void) {
    pin_out(GPIOA, 15, 0);              // CS: always selected
    pin_out(GPIOC, 13, 1);              // RS
    pin_out(GPIOC, 14, 1);              // WR
    pin_out(GPIOC, 15, 1);              // RD (never read)
    pin_out(GPIOA, 8, 1);               // RST
    GPIOB->CRL = 0x33333333;
    GPIOB->CRH = 0x33333333;

    cmd(0x01); delay_ms(150);           // software reset
    cmd(0x11); delay_ms(120);           // sleep out
    cmd(0x3A); data(0x55); delay_ms(10);    // 16-bit colour
    cmd(0x36); data(0x60);              // landscape
    cmd(0x21); delay_ms(10);            // inversion on
    cmd(0x13); delay_ms(10);            // normal mode
    lcd_fill(0, 0, LCD_W, LCD_H, C_BG);
    cmd(0x29); delay_ms(10);            // display on
    pin_out(GPIOC, 9, 0);               // backlight on
}

// Portuguese accented lower-case letters for the 5x7 font: the accent sits
// in rows 0-1 (free above lower-case letters), the cedilla in row 7.
typedef struct { uint16_t code; uint8_t col[5]; } Extra;
__attribute__((section(".fonts"))) static const Extra EXTRA[] = {
    {0xE1, {0x20, 0x54, 0x56, 0x55, 0x78}},     // a acute
    {0xE2, {0x20, 0x56, 0x55, 0x56, 0x78}},     // a circumflex
    {0xE3, {0x20, 0x56, 0x55, 0x56, 0x79}},     // a tilde
    {0xE7, {0x38, 0x44, 0x44, 0xC4, 0x20}},     // c cedilla
    {0xE9, {0x38, 0x54, 0x56, 0x55, 0x18}},     // e acute
    {0xEA, {0x38, 0x56, 0x55, 0x56, 0x18}},     // e circumflex
    {0xED, {0x00, 0x44, 0x7E, 0x41, 0x00}},     // i acute
    {0xF3, {0x38, 0x44, 0x46, 0x45, 0x38}},     // o acute
    {0xF4, {0x38, 0x46, 0x45, 0x46, 0x38}},     // o circumflex
    {0xF5, {0x38, 0x46, 0x45, 0x46, 0x39}},     // o tilde
    {0xFA, {0x3C, 0x40, 0x42, 0x21, 0x7C}},     // u acute
    {0x394, {0x70, 0x4C, 0x43, 0x4C, 0x70}},    // capital delta (cursor differences)
};

static const uint8_t* glyph5x7(unsigned code) {
    if (code >= 0x20 && code <= 0x7E) return FONT5X7[code - 0x20];
    for (unsigned i = 0; i < sizeof EXTRA / sizeof EXTRA[0]; i++)
        if (EXTRA[i].code == code) return EXTRA[i].col;
    return FONT5X7['?' - 0x20];
}

// next character of a UTF-8 string (only the 2-byte Latin-1 range is used)
static unsigned utf8_next(const char** pp) {
    const unsigned char* p = (const unsigned char*)*pp;
    unsigned c = *p++;
    if ((c & 0xE0) == 0xC0 && (*p & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
    *pp = (const char*)p;
    return c;
}

static int draw_glyph(int x, int y, const uint8_t* g, int s, uint16_t fg, uint16_t bg);

int lcd_char(int x, int y, char ch, int s, uint16_t fg, uint16_t bg) {
    return draw_glyph(x, y, glyph5x7((unsigned char)ch), s, fg, bg);
}

static int draw_glyph(int x, int y, const uint8_t* g, int s, uint16_t fg, uint16_t bg) {
    window(x, y, 6 * s, 8 * s);
    uint16_t f = (uint16_t)~fg, b = (uint16_t)~bg;
    for (int row = 0; row < 8; row++)
        for (int sy = 0; sy < s; sy++)
            for (int col = 0; col < 6; col++) {
                GPIOB->ODR = (col < 5 && ((g[col] >> row) & 1)) ? f : b;
                for (int sx = 0; sx < s; sx++) wr_pulse();
            }
    return x + 6 * s;
}

int lcd_text(int x, int y, const char* t, int s, uint16_t fg, uint16_t bg) {
    while (*t) x = draw_glyph(x, y, glyph5x7(utf8_next(&t)), s, fg, bg);
    return x;
}

int lcd_text_w(const char* t, int s) {
    int n = 0;
    while (*t) { utf8_next(&t); n++; }
    return n * 6 * s;
}

// ----------------------------------------------------------------- icons --
void icon_arrow(int x, int y, int up, uint16_t c) {
    for (int i = 0; i < 5; i++) {
        int w = 1 + 2 * i;
        lcd_fill(x + 4 - i, up ? y + i : y + 4 - i, w, 1, c);
    }
}

void icon_updown(int x, int y, uint16_t c, uint16_t bg) {
    lcd_fill(x, y, 7, 12, bg);
    lcd_fill(x + 3, y + 2, 1, 8, c);
    for (int i = 0; i < 3; i++) {
        lcd_fill(x + 3 - i, y + i, 1 + 2 * i, 1, c);
        lcd_fill(x + 3 - i, y + 11 - i, 1 + 2 * i, 1, c);
    }
}

void icon_edge(int x, int y, int rising, uint16_t c, uint16_t bg) {
    lcd_fill(x, y, 10, 12, bg);
    int lo = y + 10, hi = y + 1;
    lcd_fill(x, rising ? lo : hi, 4, 1, c);         // first level
    lcd_fill(x + 4, hi, 1, lo - hi + 1, c);         // the edge
    lcd_fill(x + 5, rising ? hi : lo, 5, 1, c);     // second level
    int ay = y + 5;                                  // arrow on the edge
    if (rising) { lcd_fill(x + 3, ay, 3, 1, c); lcd_fill(x + 2, ay + 1, 5, 1, c); }
    else        { lcd_fill(x + 2, ay, 5, 1, c); lcd_fill(x + 3, ay + 1, 3, 1, c); }
}

void icon_square(int x, int y, uint16_t c, uint16_t bg) {
    lcd_fill(x, y, 11, 12, bg);
    lcd_fill(x, y + 9, 3, 1, c);
    lcd_fill(x + 2, y + 2, 1, 8, c);
    lcd_fill(x + 2, y + 2, 4, 1, c);
    lcd_fill(x + 5, y + 2, 1, 8, c);
    lcd_fill(x + 5, y + 9, 4, 1, c);
    lcd_fill(x + 8, y + 2, 1, 8, c);
    lcd_fill(x + 8, y + 2, 3, 1, c);
}

// red X over an 11x12 icon (something switched off)
void icon_cross(int x, int y, uint16_t c) {
    for (int i = 0; i < 10; i++) {
        lcd_fill(x + i, y + 1 + i, 2, 1, c);
        lcd_fill(x + 9 - i, y + 1 + i, 2, 1, c);
    }
}

void icon_pc(int x, int y, int state, uint16_t bg) {
    // grey frame always; the screen says the state: empty = no PC,
    // amber broken link = PC but port closed, light green = sending
    uint16_t c = C_MUTED;
    lcd_fill(x, y, 16, 12, bg);
    lcd_fill(x, y, 16, 1, c); lcd_fill(x, y + 8, 16, 1, c);       // screen
    lcd_fill(x, y, 1, 9, c);  lcd_fill(x + 15, y, 1, 9, c);
    lcd_fill(x + 6, y + 9, 4, 2, c); lcd_fill(x + 4, y + 11, 8, 1, c);   // stand
    if (state == 2) lcd_fill(x + 2, y + 2, 12, 5, RGB(120, 230, 120));
    if (state == 1) {                                               // broken link
        uint16_t a = C_TRIG;
        lcd_fill(x + 3, y + 4, 3, 1, a); lcd_fill(x + 10, y + 4, 3, 1, a);
        lcd_fill(x + 7, y + 2, 1, 2, a); lcd_fill(x + 8, y + 5, 1, 2, a);
    }
}

void icon_battery(int x, int y, int level, int charging, uint16_t bg) {
    uint16_t c = level < 0 ? C_MUTED : level <= 1 ? C_RED : C_TEXT;
    lcd_fill(x, y, 22, 12, bg);
    lcd_fill(x, y + 1, 19, 1, c); lcd_fill(x, y + 10, 19, 1, c);
    lcd_fill(x, y + 1, 1, 10, c); lcd_fill(x + 18, y + 1, 1, 10, c);
    lcd_fill(x + 19, y + 4, 2, 4, c);
    if (charging) {
        // charging: amber bars filling one after another, from the current
        // level up to full, then again (the caller redraws about once a second)
        int base = level < 0 ? 0 : (level > 3 ? 3 : level);
        int n = base + (int)((g_ms / 600) % (uint32_t)(5 - base));
        for (int i = 0; i < 4 && i < n; i++) lcd_fill(x + 2 + i * 4, y + 3, 3, 6, C_TRIG);
        return;
    }
    for (int i = 0; i < 4 && i < level; i++)
        lcd_fill(x + 2 + i * 4, y + 3, 3, 6, level <= 1 ? C_RED : RGB(151, 196, 89));
}
