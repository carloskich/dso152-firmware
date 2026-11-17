// DSO152 firmware v2.1 (multimeter, averaging, trigger position, rise/fall,
// free generator frequency and duty, dangerous-voltage warning)
//
// Keys (as agreed with the user, 2026-11-16):
//   LEFT / RIGHT   pick what UP / DOWN changes: V/div, time/div, position,
//                  trigger level (highlighted in the top bar)
//   UP / DOWN      change it
//   MODE           DC / AC
//   RUN            run / stop (in Single: arm again)
//   AUTO           autoset: scale, time base, position
//   OK             menu: trigger mode, edge, probe x1/x10, generator,
//                  cursors, zero all ranges, factory zero, battery, diagnostics
// With cursors on, LEFT / RIGHT also reach cursor 1 and cursor 2.
// From 50 ms/div up the trace rolls from right to left, untriggered.
// USB: a serial port (CDC). While a program holds it open, every trace and
// the measurements go out as text lines; "K x" lines press keys (see pc_*).
//   POWER (hold 2 s) switch off
// Settings are kept in our flash page (0x0800F800), saved 2 s after the
// last change. The FNIRSI calibration page is only ever read.
#include "hw.h"
#include "lcd.h"
#include "keys.h"
#include "acq.h"
#include "frontend.h"
#include "settings.h"
#include "gen.h"
#include "usb.h"

// functions placed below the bootloader font area (FLASH), where there is room
#define LO __attribute__((section(".lotext"), noinline))

#define FW_VER "v2.1"
#define FW_AUTHOR "Carlos Santa Kich"

// ----------------------------------------------------------- geometry ---
#define GX 10
#define GY 24
#define DIVX 30
#define DIVY 22
#define NDX 10
#define NDY 8
#define GW (DIVX * NDX)         // 300
#define GH (DIVY * NDY)         // 176
#define CY (GY + GH / 2)
#define NS GW
#define NCAP (2 * NS)
#define ADC_LIMIT 1900          // counts either side of zero kept for the screen edge
#define POS_STEP (DIVY / 4)
#define POS_MAX (GH / 2)

// ------------------------------------------------------------- scales ---
static const uint16_t VDIV_MV[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
#define NVDIV (sizeof VDIV_MV / sizeof VDIV_MV[0])
static const uint32_t TDIV_US[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000,
                                   100000, 200000, 500000, 1000000, 2000000};
static const char* const TDIV_TXT[] = {"20us", "50us", "100us", "200us", "500us", "1ms",
                                       "2ms", "5ms", "10ms", "20ms", "50ms",
                                       "100ms", "200ms", "500ms", "1s", "2s"};
#define NTDIV (sizeof TDIV_US / sizeof TDIV_US[0])
#define TD_ROLL 10              // 50 ms/div and slower: roll mode

enum { P_VDIV, P_TDIV, P_POS, P_TRIG, NPARAM, P_HPOS = NPARAM, P_C1, P_C2 };
#define HPOS_STEP (DIVX / 2)
#define HPOS_MAX (NS / 2 - DIVX / 2)
enum { CUR_OFF, CUR_TIME, CUR_VOLT };

// generator steps (menu): off, then these frequencies, 50 % duty on PA0
static const uint32_t GEN_HZ[] = {0, 10, 50, 60, 100, 500, 1000, 5000, 10000, 50000, 100000};
#define NGEN (sizeof GEN_HZ / sizeof GEN_HZ[0])
#define GEN_CUSTOM NGEN                 // cfg.gen: the frequency typed on the PC (cfg.gen_hz)
#define GEN_DEFAULT 6                   // 1 kHz
static const uint8_t DUTY_STEPS[] = {10, 25, 50, 75, 90};
static const uint8_t AVG_N[4] = {1, 4, 8, 16};
enum { TM_AUTO, TM_NORMAL, TM_SINGLE };

// -------------------------------------------------------------- state ---
static Settings cfg;
static int16_t  fz_dc[FE_RANGES], fz_ac[FE_RANGES];    // factory zero
static uint8_t  s_factory_ok;
static uint8_t  s_sel = P_VDIV;
static uint8_t  s_run = 1;
static uint8_t  s_waiting;              // Normal / Single: no trigger yet
static uint32_t s_fs;
static uint8_t  s_dirty;
static uint32_t s_dirty_t;

static int32_t  m_min, m_max, m_avg, m_rms, m_duty;   // m_duty in 0.1 %, -1 unknown
static uint32_t m_freq_mhz;
static int32_t  m_trig;                 // trigger level in counts
static uint8_t  m_clip;
static int32_t  f_min, f_max, f_avg, f_trig, f_rms;
static uint8_t  f_valid;
static uint32_t m_rise, m_fall;         // 10-90 % edge times, tenths of a microsecond, 0 unknown
static uint32_t s_gen_real;             // generator frequency actually produced, mHz
static uint16_t a_buf[NS];              // averaged trace, counts x16
static uint16_t a_out[NS];              // the averaged trace in counts, for drawing
static uint8_t  a_n;                    // captures in the average so far
static int      s_start = -1, s_trig_at;   // last trace: first sample, trigger sample
static int16_t  o_hx = -1;              // trigger-position marker drawn at this grid x

static uint8_t  o_top[NS], o_bot[NS];
static int16_t  o_trig_y = -1, o_zero_y = -1;
static uint8_t  o_banner;
static int16_t  o_guide_y = -1;         // row of the guide on screen, -1 = none
static int16_t  s_cur_t[2] = {GW / 3, 2 * GW / 3};     // time cursors, grid x
static int16_t  s_cur_v[2] = {GH / 3, 2 * GH / 3};     // voltage cursors, grid y
static int16_t  o_cur[2] = {-1, -1};    // drawn position, -1 = none
static uint8_t  o_cur_kind, o_cur_sel;
static uint8_t  s_cur_redraw;           // something went over the cursors
static uint8_t  s_zero_big, s_trig_big;

static void mark_dirty(void) { s_dirty = 1; s_dirty_t = g_ms; }
static int pf(void) { return cfg.probe10 ? 10 : 1; }
static uint32_t gen_hz_now(void) {
    if (cfg.gen == GEN_CUSTOM) return cfg.gen_hz;
    return GEN_HZ[cfg.gen < NGEN ? cfg.gen : 0];
}
static int gen_duty_now(void) { return cfg.gen_duty >= 1 && cfg.gen_duty <= 99 ? cfg.gen_duty : 50; }
static void gen_apply(void) { s_gen_real = gen_set(gen_hz_now(), (uint32_t)gen_duty_now()); }
static int16_t* zero_tab(void) { return cfg.dc ? cfg.zero_dc : cfg.zero_ac; }
static int32_t zero(void) { return zero_tab()[fe_range()]; }
static int32_t cpv(void) { return (int32_t)fe_counts_per_volt(fe_range()); }

// --------------------------------------------------------- formatting ---
static char* u2s(uint32_t v, char* p) {
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static char* frac(uint32_t v, int digits, char* p) {
    char t[8];
    for (int i = digits - 1; i >= 0; i--) { t[i] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < digits; i++) *p++ = t[i];
    *p = 0;
    return p;
}
static char* fmt_mv(int32_t mv, char* p) {
    if (mv < 0) { *p++ = '-'; mv = -mv; }
    if (mv >= 10000)     { p = u2s(mv / 1000, p); *p++ = '.'; p = frac((mv % 1000) / 100, 1, p); }
    else if (mv >= 1000) { p = u2s(mv / 1000, p); *p++ = '.'; p = frac((mv % 1000) / 10, 2, p); }
    else                 { p = u2s(mv, p); *p++ = 'm'; }
    *p++ = 'V'; *p = 0;
    return p;
}
static void fmt_div(int32_t mv, char* p) {      // V/div label: 10mV .. 200V
    if (mv >= 1000) { p = u2s(mv / 1000, p); *p++ = 'V'; *p = 0; }
    else            { p = u2s(mv, p); *p++ = 'm'; *p++ = 'V'; *p = 0; }
}
static char* fmt3(uint32_t v, char* p) {        // v in thousandths, three significant digits
    if (v >= 100000)     { p = u2s(v / 1000, p); }
    else if (v >= 10000) { p = u2s(v / 1000, p); *p++ = '.'; p = frac((v % 1000) / 100, 1, p); }
    else                 { p = u2s(v / 1000, p); *p++ = '.'; p = frac((v % 1000) / 10, 2, p); }
    return p;
}
static char* put(char* p, const char* t) { while (*t) *p++ = *t++; *p = 0; return p; }
static void fmt_freq(uint32_t mhz, char* p) {   // three significant digits
    if (!mhz) { p[0] = '-'; p[1] = '-'; p[2] = 0; return; }
    uint32_t hz = mhz / 1000;
    char unit = 0;
    uint32_t v = mhz;                       // value in units of 1/1000
    if (hz >= 1000000) { unit = 'M'; v = hz / 1000; }
    else if (hz >= 1000) { unit = 'k'; v = hz; }
    p = fmt3(v, p);
    if (unit) *p++ = unit;
    *p++ = 'H'; *p++ = 'z'; *p = 0;
}
static void fmt_time(uint32_t t10, char* p) {   // t10: tenths of a microsecond
    if (t10 < 10000)         { p = fmt3(t10 * 100, p); put(p, "us"); }
    else if (t10 < 10000000) { p = fmt3(t10 / 10, p); put(p, "ms"); }
    else                     { p = fmt3(t10 / 10000, p); put(p, "s"); }
}

// ------------------------------------------------------------ scaling ---
static uint8_t range_for(void) {
    // most sensitive range whose linear span covers the visible window
    uint64_t need_px = (uint64_t)(GH / 2 + (cfg.pos < 0 ? -cfg.pos : cfg.pos));
    for (uint8_t r = 0; r < FE_RANGES; r++)
        if (need_px * VDIV_MV[cfg.vdiv] * fe_counts_per_volt(r) <= (uint64_t)ADC_LIMIT * 1000u * (GH / 2))
            return r;
    return FE_RANGES - 1;
}
static int32_t counts_to_mv(int32_t c) { return c * 1000 / cpv(); }   // probe x1
static int32_t mv_to_counts(int32_t mv) { return mv * cpv() / 1000; }
static int y_of(int32_t s) {
    int32_t d = (s - zero()) * (DIVY * 1000) / ((int32_t)VDIV_MV[cfg.vdiv] * cpv());
    int32_t y = CY - cfg.pos - d;
    if (y < GY) y = GY;
    if (y > GY + GH) y = GY + GH;
    return (int)y;
}
static int32_t mv_of_f(int32_t f16) {           // smoothed counts -> mV (probe x1), rounded
    int32_t mv = (f16 - zero() * 16) * 1000 / (cpv() * 16);
    int32_t res = 1000 / cpv();
    int32_t step = res >= 100 ? 100 : res >= 10 ? 10 : 1;
    int32_t half = step / 2;
    return mv >= 0 ? (mv + half) / step * step : -((-mv + half) / step * step);
}

static void apply_scales(void) {
    fe_set_range(range_for());
    s_fs = 30000000u / TDIV_US[cfg.tdiv];
    if (s_fs > ACQ_MAX_HZ) s_fs = ACQ_MAX_HZ;
    f_valid = 0;
}

// --------------------------------------------------------------- grid ---
static uint16_t grid_col_colour(int gx) {
    if (gx == GW / 2) return C_GRID2;
    return (gx % DIVX == 0) ? C_GRID : 0;
}
static void bg_col(int gx, int y0, int y1) {
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    uint16_t vc = grid_col_colour(gx);
    if (vc) { lcd_fill(GX + gx, y0, 1, y1 - y0 + 1, vc); return; }
    lcd_fill(GX + gx, y0, 1, y1 - y0 + 1, C_BG);
    int first = ((y0 - GY + DIVY - 1) / DIVY) * DIVY + GY;
    for (int y = first; y <= y1; y += DIVY)
        lcd_pixel(GX + gx, y, (y == CY) ? C_GRID2 : ((gx & 1) ? C_BG : C_GRID));
}
static void draw_grid(void) {
    for (int gx = 0; gx <= GW; gx++) bg_col(gx, GY, GY + GH);
    for (int i = 0; i < NS; i++) o_top[i] = o_bot[i] = 0xFF;
    o_trig_y = o_zero_y = -1;
    o_banner = 0;
    o_guide_y = -1;
    o_cur[0] = o_cur[1] = -1;
    o_hx = -1;
}

static void marker(int x, int y, int dir, uint16_t c, int big) {
    int n = big ? 6 : 4;
    for (int i = 0; i < n; i++) {
        int h = 2 * n - 1 - 2 * i;
        lcd_fill(dir > 0 ? x + i : x - i, y - h / 2, 1, h, c);
    }
}
static uint32_t s_guide_t;              // when the zero / trigger level was last moved
static uint8_t  s_frame_new;            // a trace was drawn since the guide was
static void guide_line(int y, uint16_t c) {
    for (int x = 0; x < GW; x += 6) lcd_fill(GX + x, y, 3, 1, c);
}
static int s_mark_x = NS / 2;            // where the trigger sample is on the screen
static void hmark_erase(void) {
    if (o_hx < 0) return;
    for (int x = o_hx - 3; x <= o_hx + 3; x++) {
        if (x < 0 || x >= NS) continue;
        bg_col(x, GY, GY + 4);
        if (o_top[x] != 0xFF && o_top[x] <= 4) {
            int b = o_bot[x] > 4 ? 4 : o_bot[x];
            lcd_fill(GX + x, GY + o_top[x], 1, b - o_top[x] + 1, C_WAVE);
        }
    }
    o_hx = -1;
}
static void hmark_draw(void) {
    int x = cfg.tdiv >= TD_ROLL ? -1 : s_mark_x;
    if (x < 0 || x >= NS) { hmark_erase(); return; }
    if (x != o_hx) hmark_erase();
    int big = s_sel == P_HPOS;
    for (int i = 0; i < 4; i++) {
        int w = 7 - 2 * i;
        lcd_fill(GX + x - w / 2, GY + i, w, 1, big ? C_TEXT : C_TRIG);
    }
    o_hx = (int16_t)x;
}
static void draw_markers(void) {
    hmark_draw();
    int zy = y_of(zero());
    int zbig = s_sel == P_POS, tbig = s_sel == P_TRIG;
    if (zy != o_zero_y || zbig != s_zero_big) {
        lcd_fill(0, GY - 6, GX - 1, GH + 13, C_BG);
        marker(zbig ? 2 : 4, zy, +1, C_WAVE, zbig);
        o_zero_y = (int16_t)zy;
        s_zero_big = (uint8_t)zbig;
    }
    int ty = y_of(m_trig);
    if (ty != o_trig_y || tbig != s_trig_big) {
        lcd_fill(GX + GW + 2, GY - 6, LCD_W - (GX + GW + 2), GH + 13, C_BG);
        marker(LCD_W - (tbig ? 2 : 4), ty, -1, C_TRIG, tbig);
        o_trig_y = (int16_t)ty;
        s_trig_big = (uint8_t)tbig;
    }
}

// Put back what the guide covered: grid / background, and the trace where
// its column segment crosses that row.
static void guide_erase(void) {
    if (o_guide_y < 0) return;
    int y = o_guide_y;
    for (int x = 0; x < GW; x++) {
        if (x % 6 >= 3) continue;                   // only the dashes were drawn
        bg_col(x, y, y);
        if (o_top[x] != 0xFF && y >= GY + o_top[x] && y <= GY + o_bot[x]) lcd_pixel(GX + x, y, C_WAVE);
    }
    o_guide_y = -1;
    s_cur_redraw = 1;
}
// Dashed guide across the grid for 1.5 s after the zero or trigger level moved.
static void guide_update(void) {
    int want = -1;
    uint16_t c = 0;
    if (g_ms - s_guide_t < 1500) {
        if (s_sel == P_POS)  { want = y_of(zero()); c = C_WAVE; }
        if (s_sel == P_TRIG) { want = y_of(m_trig); c = C_TRIG; }
    }
    if (want != o_guide_y) {
        guide_erase();
        if (want >= 0) { guide_line(want, c); o_guide_y = (int16_t)want; }
    } else if (want >= 0 && s_frame_new) {
        guide_line(want, c);                        // a new trace went over it
    }
    s_frame_new = 0;
}

// --------------------------------------------------------------- cursors ---
// Two dashed purple lines, vertical (time) or horizontal (volts). The one
// being moved has long dashes, the other dots. The trace stays on top.
#define C_CUR RGB(186, 140, 250)
#define C_CUR_SEL RGB(255, 80, 200)         // the cursor being moved: bright pink, long dashes
static int nsel(void) { return cfg.cursors ? P_C2 + 1 : P_HPOS + 1; }
static int on_trace(int x, int y) {
    return o_top[x] != 0xFF && y >= GY + o_top[x] && y <= GY + o_bot[x];
}
static int in_banner(int y) { return o_banner && y < GY + 16; }
static void cursor_line(int kind, int pos, int sel, int erase) {
    if (kind == CUR_TIME) {
        int x = pos >= GW ? GW - 1 : pos;
        if (erase) {
            bg_col(x, GY, GY + GH);
            if (o_top[x] != 0xFF) lcd_fill(GX + x, GY + o_top[x], 1, o_bot[x] - o_top[x] + 1, C_WAVE);
            return;
        }
        for (int y = GY; y <= GY + GH; y++)
            if ((y - GY) % 4 < (sel ? 3 : 1) && !on_trace(x, y) && !in_banner(y)) lcd_pixel(GX + x, y, sel ? C_CUR_SEL : C_CUR);
    } else {
        int y = GY + pos;
        for (int x = 0; x < GW; x++) {
            if (erase) { bg_col(x, y, y); if (on_trace(x, y)) lcd_pixel(GX + x, y, C_WAVE); continue; }
            if (x % 4 < (sel ? 3 : 1) && !on_trace(x, y) && !in_banner(y)) lcd_pixel(GX + x, y, sel ? C_CUR_SEL : C_CUR);
        }
    }
}
static void cursor_update(void) {
    int kind = cfg.cursors, sel = s_sel >= P_C1 ? s_sel - P_C1 + 1 : 0;
    int16_t want[2] = {-1, -1};
    if (kind) for (int i = 0; i < 2; i++) want[i] = kind == CUR_TIME ? s_cur_t[i] : s_cur_v[i];
    int changed = kind != o_cur_kind || sel != o_cur_sel || want[0] != o_cur[0] || want[1] != o_cur[1];
    if (changed) {
        for (int i = 0; i < 2; i++) if (o_cur[i] >= 0) cursor_line(o_cur_kind, o_cur[i], 0, 1);
    }
    if (kind && (changed || s_cur_redraw))
        for (int i = 0; i < 2; i++) cursor_line(kind, want[i], sel == i + 1, 0);
    o_cur[0] = want[0]; o_cur[1] = want[1];
    o_cur_kind = (uint8_t)kind; o_cur_sel = (uint8_t)sel;
    s_cur_redraw = 0;
}
static int32_t cursor_mv(int i) {               // voltage at a horizontal cursor, probe included
    return (int32_t)(GH / 2 - cfg.pos - s_cur_v[i]) * (int32_t)VDIV_MV[cfg.vdiv] * pf() / DIVY;
}

// ------------------------------------------------------------ top bar ---
// state pill | V/div | time/div | position | trigger | DC | generator, PC, battery
#define TB_H (GY - 2)
static const int PX[NPARAM] = {44, 88, 132, 184};      // x of each selectable field
static const int PW[NPARAM] = {42, 42, 50, 48};
static const uint16_t PCOL[NPARAM] = {C_WAVE, C_TIME, C_WAVE, C_TRIG};

static void param_text(int p, char* b) {
    switch (p) {
    case P_VDIV: fmt_div((int32_t)VDIV_MV[cfg.vdiv] * pf(), b); break;
    case P_TDIV: { const char* t = TDIV_TXT[cfg.tdiv]; while ((*b++ = *t++)) {} } break;
    case P_POS:  fmt_mv(cfg.pos * (int32_t)VDIV_MV[cfg.vdiv] / DIVY * pf(), b); break;
    case P_TRIG:
        if (cfg.trig_auto) { const char* t = "auto"; while ((*b++ = *t++)) {} }
        else fmt_mv(cfg.trig_mv * pf(), b);
        break;
    }
}
static int pc_state(void) { return usb_state(); }
static uint8_t s_pc_drawn = 0xFF;
static uint16_t s_bat_mv;                       // battery, mV (estimate)
static uint16_t s_bat_pin;                      // mV at PA7
static uint8_t  s_bat_chg;
static uint32_t s_bat_t;
static int32_t  s_bat_prev = -1;
// Divider measured on the unit (2026-11-16): PA7 = 2.09 V without charger,
// 2.11 V with it, with a nearly full cell -> 1:1 divider (ratio 2.0).
static uint16_t bat_ratio(void) {               // x1000
    return (cfg.bat_ratio >= 1500 && cfg.bat_ratio <= 3000) ? cfg.bat_ratio : 2000;
}
static void battery_poll(void) {                // between captures, about once a second
    if (g_ms - s_bat_t < 1000 && s_bat_prev >= 0) return;
    s_bat_t = g_ms;
    uint16_t vref = acq_read_channel(17);
    uint16_t raw = acq_read_channel(7);
    uint32_t vdd = vref ? 1200u * 4095u / vref : 3300u;
    s_bat_pin = (uint16_t)(raw * vdd / 4095u);
    s_bat_mv = (uint16_t)((uint32_t)s_bat_pin * bat_ratio() / 1000u);
    int32_t now = s_bat_pin;
    if (s_bat_prev >= 0) {
        if (now - s_bat_prev >= 10) s_bat_chg = 1;          // charger plugged in (~17 mV seen)
        if (s_bat_prev - now >= 10) s_bat_chg = 0;          // unplugged / finished
    }
    s_bat_prev = now;
}
// Li-ion open-circuit voltage -> charge, piecewise linear
static int batt_percent(void) {
    static const uint16_t MV[] = {3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
    static const uint8_t  PC[] = {0, 3, 12, 30, 48, 63, 78, 90, 100};
    int v = s_bat_mv;
    if (v <= MV[0]) return 0;
    for (int i = 1; i < 9; i++)
        if (v <= MV[i]) return PC[i - 1] + (PC[i] - PC[i - 1]) * (v - MV[i - 1]) / (MV[i] - MV[i - 1]);
    return 100;
}
static int batt_level(void) {
    if (!s_bat_mv) return -1;
    return s_bat_mv > 4000 ? 4 : s_bat_mv > 3850 ? 3 : s_bat_mv > 3720 ? 2 : s_bat_mv > 3600 ? 1 : 0;
}
// Read the battery when due and refresh its icon (animated while charging).
// Only call while the ADC is not sampling.
static void battery_tick(void) {
    uint8_t lv = (uint8_t)batt_level(), ch = s_bat_chg;
    uint32_t before = s_bat_t;
    battery_poll();
    if (s_bat_t == before) return;                  // not due yet
    if (s_bat_chg || lv != (uint8_t)batt_level() || ch != s_bat_chg)
        icon_battery(294, 5, batt_level(), s_bat_chg, C_BAR);
}

static void draw_top(void) {
    char b[20];
    lcd_fill(0, 0, LCD_W, TB_H, C_BAR);
    uint16_t sc = !s_run ? C_RED : s_waiting ? C_TRIG : C_WAVED;
    const char* st = !s_run ? "STOP" : cfg.tdiv >= TD_ROLL ? "ROLL" : cfg.trig_mode == TM_NORMAL ? "NORM"
                   : cfg.trig_mode == TM_SINGLE ? "SNGL" : cfg.avg ? "MED" : "RUN";
    lcd_fill(3, 4, 36, TB_H - 8, sc);
    lcd_text(3 + (36 - lcd_text_w(st, 1)) / 2, 7, st, 1, C_TEXT, sc);
    for (int p = 0; p < NPARAM; p++) {
        uint16_t bg = (p == s_sel) ? C_BOX : C_BAR;
        uint16_t fg = (p == P_VDIV && cfg.probe10) ? C_TRIG : PCOL[p];
        int x = PX[p];
        lcd_fill(x - 2, 1, PW[p], TB_H - 2, bg);
        if (p == s_sel) lcd_fill(x - 2, TB_H - 3, PW[p], 2, PCOL[p]);
        if (p == P_POS)  { icon_updown(x, 5, fg, bg); x += 9; }
        if (p == P_TRIG) { icon_edge(x, 5, cfg.rising, fg, bg); x += 13; }
        param_text(p, b);
        lcd_text(x, 7, b, 1, fg, bg);
    }
    lcd_text(238, 7, cfg.dc ? "DC" : "AC", 1, C_TEXT, C_BAR);
    icon_square(256, 5, gen_hz_now() ? C_TIME : C_MUTED, C_BAR);
    if (!gen_hz_now()) icon_cross(256, 5, C_RED);            // generator off
    s_pc_drawn = (uint8_t)pc_state();
    icon_pc(272, 5, s_pc_drawn, C_BAR);
    icon_battery(294, 5, batt_level(), s_bat_chg, C_BAR);
}

// --------------------------------------------------------- bottom bar ---
#define BB_Y (GY + GH + 2)
static uint8_t s_hide_labels, o_alert_row;    // footer: warning shown in the label row
static uint32_t isqrt(uint32_t ms);
static int32_t rms_mv(void);
static void meas_c(int x, const char* label, const char* val, uint16_t lc) {
    if (!s_hide_labels) {
        lcd_fill(x, BB_Y + 8, 60, 8, C_BAR);
        lcd_text(x, BB_Y + 8, label, 1, lc, C_BAR);
    }
    int e = lcd_text(x, BB_Y + 22, val, 1, C_TEXT, C_BAR);
    lcd_fill(e, BB_Y + 22, x + 63 - e, 8, C_BAR);
}
static void meas(int x, const char* label, const char* val) { meas_c(x, label, val, C_MUTED); }
static uint16_t cur_label_c(void) { return s_sel >= P_C1 ? C_CUR_SEL : C_CUR; }

// Dangerous voltage at the probe tip (as a multimeter would warn): DC mean
// 60 V or more, or an AC part of 30 V RMS / 42 V peak. With the probe set to
// x1, 35 V or more is over what the input is meant for: use x10.
// Returns 0, 1 dangerous, 2 over the x1 limit. Values in mV at the tip.
static LO int alert_of(int32_t avg, int32_t rms, int32_t mx, int32_t mn) {
    int32_t pk = mx > -mn ? mx : -mn;
    if (!cfg.probe10 && pk >= 35000) return 2;
    int32_t a = (avg < 0 ? -avg : avg) / 10, r = (rms < 0 ? -rms : rms) / 10;    // 10 mV units
    uint32_t acr = r > a ? isqrt((uint32_t)(r * r - a * a)) : 0;
    if (a >= 6000 || acr >= 3000 || (mx - mn) / 2 >= 42000) return 1;
    return 0;
}
static const char* const ALERT_TXT[3] = {"", "\u26A0 Tensão perigosa", "\u26A0 Acima do limite: use x10"};
static int scope_alert(void) {
    if (!f_valid) return 0;
    int p = pf();
    return alert_of(mv_of_f(f_avg) * p, rms_mv() * p, mv_of_f(f_max) * p, mv_of_f(f_min) * p);
}


// What each of the five places of the bottom bar shows (menu: Medidas na tela)
enum { MS_DEF, MS_VPP, MS_FREQ, MS_PER, MS_DUTY, MS_VMED, MS_VRMS, MS_VMAX, MS_VMIN, MS_RISE, MS_FALL, NMS };
static const char* const MS_LABEL[NMS] = {"", "VPP", "FREQ", "PER", "DUTY", "VMED", "VRMS", "VMAX", "VMIN", "TSUB", "TDES"};
static const char* const MS_NAME[NMS] = {"", "Pico a pico", "Frequência", "Período", "Duty cycle",
                                         "Média", "RMS", "Máximo", "Mínimo", "Subida", "Descida"};
static const uint8_t MS_DEFAULT[5] = {MS_VPP, MS_FREQ, MS_VMED, MS_VMAX, MS_VMIN};
static uint8_t meas_id(int k) { uint8_t m = cfg.meas[k]; return (m && m < NMS) ? m : MS_DEFAULT[k]; }
static int32_t rms_mv(void) { return f_rms * 1000 / (cpv() * 16); }
static void meas_value(uint8_t id, char* b) {
    int p = pf();
    switch (id) {
    case MS_VPP:  fmt_mv((mv_of_f(f_max) - mv_of_f(f_min)) * p, b); break;
    case MS_FREQ: fmt_freq(m_freq_mhz, b); break;
    case MS_PER:
        if (m_freq_mhz >= 1000) fmt_time((uint32_t)(10000000000ull / m_freq_mhz), b);
        else put(b, "--");
        break;
    case MS_DUTY:
        if (m_duty < 0) put(b, "--");
        else { char* q = u2s((uint32_t)m_duty / 10, b); *q++ = '.'; q = frac((uint32_t)m_duty % 10, 1, q); put(q, "%"); }
        break;
    case MS_VMED: fmt_mv(mv_of_f(f_avg) * p, b); break;
    case MS_VRMS: fmt_mv(rms_mv() * p, b); break;
    case MS_VMAX: fmt_mv(mv_of_f(f_max) * p, b); break;
    case MS_RISE: case MS_FALL: {
        uint32_t t = id == MS_RISE ? m_rise : m_fall;
        if (t) fmt_time(t, b); else put(b, "--");
        break; }
    default:      fmt_mv(mv_of_f(f_min) * p, b); break;
    }
}
static void draw_bottom(void) {
    static const int SX[5] = {6, 70, 134, 198, 262};
    char b[16];
    // the label row alternates with a red warning every 2 s; the values stay
    int al = scope_alert(), show = al && !((g_ms / 2000) & 1);
    if (show) {
        lcd_fill(0, BB_Y + 4, LCD_W, 15, C_RED);
        lcd_text(8, BB_Y + 8, ALERT_TXT[al], 1, C_TEXT, C_RED);
    } else if (o_alert_row) lcd_fill(0, BB_Y + 4, LCD_W, 15, C_BAR);
    o_alert_row = (uint8_t)show;
    s_hide_labels = (uint8_t)show;
    int nuser = cfg.cursors == CUR_VOLT ? 2 : cfg.cursors == CUR_TIME ? 3 : 5;
    for (int k = 0; k < nuser; k++) { uint8_t id = meas_id(k); meas_value(id, b); meas(SX[k], MS_LABEL[id], b); }
    if (cfg.cursors == CUR_VOLT) {
        fmt_mv(cursor_mv(0), b);                        meas_c(134, "C1", b, cur_label_c());
        fmt_mv(cursor_mv(1), b);                        meas_c(198, "C2", b, cur_label_c());
        int32_t d = cursor_mv(1) - cursor_mv(0);
        fmt_mv(d < 0 ? -d : d, b);                      meas_c(262, "\u0394V", b, cur_label_c());
        return;
    }
    if (cfg.cursors == CUR_TIME) {
        int dx = s_cur_t[1] - s_cur_t[0];
        if (dx < 0) dx = -dx;
        uint32_t t10 = (uint32_t)((uint64_t)dx * TDIV_US[cfg.tdiv] * 10u / DIVX);
        fmt_time(t10, b);                               meas_c(198, "\u0394T", b, cur_label_c());
        fmt_freq(t10 ? (uint32_t)(10000000000ull / t10) : 0, b);
        meas_c(262, "1/\u0394T", b, cur_label_c());
    }
    if (s_sel == P_HPOS) {                          // trigger position, while it is being moved
        int32_t px = cfg.hpos;
        uint32_t t10 = (uint32_t)((uint64_t)(px < 0 ? -px : px) * TDIV_US[cfg.tdiv] * 10u / DIVX);
        char* q = b;
        if (px < 0) *q++ = '-';
        fmt_time(t10, q);
        meas_c(262, "DISPARO", b, C_TRIG);
    }
    s_hide_labels = 0;
}

static void toast(const char* t, uint16_t c) {
    int w = lcd_text_w(t, 1) + 28;
    int x = (LCD_W - w) / 2, y = GY + GH / 2 - 11;
    lcd_fill(x - 1, y - 1, w + 2, 24, C_GRID2);
    lcd_fill(x, y, w, 22, C_BAR);
    lcd_text(x + 14, y + 7, t, 1, c, C_BAR);
}

// Out of range: red banner at the top centre of the grid, arrow up or down.
static void banner(void) {
    if (!m_clip) {
        if (o_banner) draw_grid();              // erase it (the trace is redrawn next frame)
        return;
    }
    const char* t = "FORA DA ESCALA";
    int w = lcd_text_w(t, 1) + 26;
    int x = GX + (GW - w) / 2, y = GY + 1;
    lcd_fill(x, y, w, 14, C_RED);
    icon_arrow(x + 6, y + 5, m_clip & 1, C_TEXT);
    lcd_text(x + 20, y + 3, t, 1, C_TEXT, C_RED);
    o_banner = m_clip;
}

// ------------------------------------------------------------- signal ---
// Returns the first sample to show, or -1 when Normal/Single found no trigger.
// minimum, maximum, mean and frequency (mid-level crossings) of n samples
static uint32_t isqrt(uint32_t ms) {
    uint32_t r = 0;
    for (uint32_t bit = 1u << 30; bit; bit >>= 2) {
        if (ms >= r + bit) { ms -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
    }
    return r;
}
// 10 % -> 90 % (rise) and 90 % -> 10 % (fall), averaged over the edges seen
static LO void edges(const uint16_t* s, int cnt, int32_t mn, int32_t mx) {
    m_rise = m_fall = 0;
    int32_t span = mx - mn;
    if (span < 24) return;
    int32_t lo = mn + span / 10, hi = mx - span / 10;
    int state = 0, last_lo = -1, last_hi = -1;          // state 1 = was low, 2 = was high
    uint32_t rs = 0, rn = 0, fs = 0, fn = 0;
    for (int i = 0; i < cnt; i++) {
        int v = s[i];
        if (v <= lo) {
            if (state == 2 && last_hi >= 0) { fs += (uint32_t)(i - last_hi); fn++; }
            state = 1; last_lo = i;
        } else if (v >= hi) {
            if (state == 1 && last_lo >= 0) { rs += (uint32_t)(i - last_lo); rn++; }
            state = 2; last_hi = i;
        }
    }
    if (rn) m_rise = (uint32_t)((uint64_t)rs * 10000000u / ((uint64_t)rn * s_fs));
    if (fn) m_fall = (uint32_t)((uint64_t)fs * 10000000u / ((uint64_t)fn * s_fs));
    if (rn && !m_rise) m_rise = 1;
    if (fn && !m_fall) m_fall = 1;
}

static void stats(const uint16_t* s, int cnt) {
    int32_t mn = 4095, mx = 0, sum = 0;
    for (int i = 0; i < cnt; i++) {
        int v = s[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    m_min = mn; m_max = mx; m_avg = cnt ? sum / cnt : 0;
    // true RMS (DC included), from the zero of the range
    uint64_t sq = 0;
    int32_t z = zero();
    for (int i = 0; i < cnt; i++) { int32_t d = (int32_t)s[i] - z; sq += (uint64_t)((int64_t)d * d); }
    m_rms = (int32_t)isqrt(cnt ? (uint32_t)(sq / (uint32_t)cnt) : 0);
    edges(s, cnt, mn, mx);
    int32_t span = mx - mn, mid = (mn + mx) / 2;
    int32_t hy = span / 10; if (hy < 3) hy = 3;
    m_freq_mhz = 0;
    m_duty = -1;
    if (span >= 24) {
        int armed = 0, first = -1, last = -1, n = 0;
        for (int i = 0; i < cnt; i++) {
            if (s[i] < mid - hy) armed = 1;
            if (armed && s[i] >= mid) { armed = 0; if (first < 0) first = i; last = i; n++; }
        }
        if (n >= 2 && last > first) {
            m_freq_mhz = (uint32_t)((uint64_t)(n - 1) * s_fs * 1000u / (uint32_t)(last - first));
            int hi = 0;                                     // whole periods only
            for (int i = first; i < last; i++) if (s[i] >= mid) hi++;
            m_duty = hi * 1000 / (last - first);
        }
    }
}

static int analyse(void) {
    const uint16_t* s = g_acq;
    stats(s, NCAP);
    int32_t span = m_max - m_min, mid = (m_min + m_max) / 2;
    int32_t hy = span / 10; if (hy < 3) hy = 3;

    // trigger
    m_trig = cfg.trig_auto ? mid : zero() + mv_to_counts(cfg.trig_mv);
    int trig = -1;
    int left = NS / 2 + cfg.hpos;               // trigger sample = first sample + left
    if (span >= 12 || !cfg.trig_auto) {
        int armed = 0, last = NCAP - NS + left;
        for (int i = 1; i <= last; i++) {
            int v = s[i];
            if (cfg.rising) {
                if (v < m_trig - hy) armed = 1;
                if (armed && v >= m_trig) { if (i >= left) { trig = i; break; } armed = 0; }
            } else {
                if (v > m_trig + hy) armed = 1;
                if (armed && v <= m_trig) { if (i >= left) { trig = i; break; } armed = 0; }
            }
        }
    }
    if (trig >= 0) { s_trig_at = trig; return trig - left; }
    s_trig_at = (NCAP - NS) / 2 + left;
    return cfg.trig_mode == TM_AUTO ? (NCAP - NS) / 2 : -1;
}

static void smooth(void) {
    if (!f_valid) { f_min = m_min * 16; f_max = m_max * 16; f_avg = m_avg * 16; f_trig = m_trig * 16; f_rms = m_rms * 16; f_valid = 1; return; }
    f_rms  += (m_rms * 16 - f_rms) / 4;
    f_min  += (m_min * 16 - f_min) / 4;
    f_max  += (m_max * 16 - f_max) / 4;
    f_avg  += (m_avg * 16 - f_avg) / 4;
    f_trig += (m_trig * 16 - f_trig) / 4;
}

// Trace of NS samples; the columns left of 'from' have no data (roll mode
// filling up) and are left empty.
static void draw_trace(const uint16_t* s, int from) {
    int py = y_of(s[from < NS ? from : 0]);
    m_clip = 0;
    s_cur_redraw = 1;
    for (int i = 0; i < NS; i++) {
        if (i < from) {
            if (o_top[i] != 0xFF) bg_col(i, GY + o_top[i], GY + o_bot[i]);
            o_top[i] = o_bot[i] = 0xFF;
            continue;
        }
        int y = y_of(s[i]);
        if (y <= GY) m_clip |= 1;
        if (y >= GY + GH) m_clip |= 2;
        int top = y < py ? y : py, bot = y < py ? py : y;
        if (o_top[i] != 0xFF) bg_col(i, GY + o_top[i], GY + o_bot[i]);
        lcd_fill(GX + i, top, 1, bot - top + 1, C_WAVE);
        o_top[i] = (uint8_t)(top - GY);
        o_bot[i] = (uint8_t)(bot - GY);
        py = y;
    }
}

// ------------------------------------------------------------- PC link ---
// One text line per trace while a program has the port open:
//   W <Sa/s> <us/div> <mV/div> <probe> <dc> <zero> <counts/V> <trigger> <roll> <n>
//   D <n samples, three hex digits each, raw ADC counts>
// and with the bottom bar, in mV / mHz at the probe tip:
//   M <vpp> <freq> <mean> <max> <min> <rms> <duty 0.1 %, -1 unknown>
// and four times a second the state, to draw the same screen on the PC:
//   S <run> <waiting> <trig mode> <selected> <position px> <rising> <trig auto>
//     <trig mV> <cursors> <time cur 1> <time cur 2> <volt cur 1> <volt cur 2>
//     <generator> <battery level> <charging> <5 bottom bar measurements>
//     <duty %> <generator real mHz> <generator step> <averaging> <trigger x px>
//     <typed generator Hz>
// and what is open on the instrument:
//   U 0                      the scope screen
//   U 1 <item>\t<title>\t<name>\t<value>...   a menu, <item> highlighted
//   U 2 <title>              another page (diagnostics, a dialog)
//   U 3                      the multimeter, with its values in
//   V <vdc> <vrms> <freq mHz> <vpp> <vmax> <vmin> (uV at the tip) <focus>
//     <hold> <fixed range> <full scale mV> <probe> <dc> <alert> <over range>
// The PC sends "K x" to press a key: U D L R (arrows) O (OK) M (MODE)
// A (AUTO) S (RUN/STOP).
static char s_pcbuf[1000];
static char* put_u(char* p, uint32_t v) { p = u2s(v, p); *p++ = ' '; return p; }
static char* put_i(char* p, int32_t v) { if (v < 0) { *p++ = '-'; v = -v; } return put_u(p, (uint32_t)v); }
static const uint16_t* s_last_p;        // the trace on the screen, sent again while stopped
static int s_last_n;
static void pc_frame(const uint16_t* s, int n) {
    s_last_p = s; s_last_n = n;
    if (usb_state() != 2) return;
    static const char H[] = "0123456789ABCDEF";
    char* p = s_pcbuf;
    *p++ = 'W'; *p++ = ' ';
    p = put_u(p, s_fs); p = put_u(p, TDIV_US[cfg.tdiv]); p = put_u(p, VDIV_MV[cfg.vdiv]);
    p = put_u(p, (uint32_t)pf()); p = put_u(p, cfg.dc); p = put_u(p, (uint32_t)zero());
    p = put_u(p, (uint32_t)cpv()); p = put_i(p, m_trig); p = put_u(p, cfg.tdiv >= TD_ROLL);
    p = u2s((uint32_t)n, p);
    *p++ = '\n'; *p++ = 'D'; *p++ = ' ';
    for (int i = 0; i < n; i++) { uint16_t v = s[i]; *p++ = H[(v >> 8) & 15]; *p++ = H[(v >> 4) & 15]; *p++ = H[v & 15]; }
    *p++ = '\n';
    usb_write(s_pcbuf, (int)(p - s_pcbuf));
}
static void pc_meas(void) {
    if (usb_state() != 2) return;
    char b[80], *p = b;
    int k = pf();
    *p++ = 'M'; *p++ = ' ';
    p = put_i(p, (mv_of_f(f_max) - mv_of_f(f_min)) * k); p = put_u(p, m_freq_mhz);
    p = put_i(p, mv_of_f(f_avg) * k); p = put_i(p, mv_of_f(f_max) * k); p = put_i(p, mv_of_f(f_min) * k);
    p = put_i(p, rms_mv() * k); p = put_i(p, m_duty);
    p = put_u(p, m_rise); p = put_u(p, m_fall);
    p[-1] = '\n';
    usb_write(b, (int)(p - b));
}
static void pc_status(void) {
    if (usb_state() != 2) return;
    char b[180], *p = b;
    *p++ = 'S'; *p++ = ' ';
    p = put_u(p, s_run); p = put_u(p, s_waiting); p = put_u(p, cfg.trig_mode); p = put_u(p, s_sel);
    p = put_i(p, cfg.pos); p = put_u(p, cfg.rising); p = put_u(p, cfg.trig_auto); p = put_i(p, cfg.trig_mv);
    p = put_u(p, cfg.cursors);
    p = put_i(p, s_cur_t[0]); p = put_i(p, s_cur_t[1]); p = put_i(p, s_cur_v[0]); p = put_i(p, s_cur_v[1]);
    p = put_u(p, gen_hz_now()); p = put_i(p, batt_level()); p = put_u(p, s_bat_chg);
    for (int k = 0; k < 5; k++) p = put_u(p, meas_id(k));
    p = put_u(p, (uint32_t)gen_duty_now()); p = put_u(p, s_gen_real); p = put_u(p, cfg.gen);
    p = put_u(p, cfg.avg); p = put_i(p, cfg.hpos); p = put_u(p, cfg.gen_hz);
    p[-1] = '\n';
    usb_write(b, (int)(p - b));
}
static void pc_hello(void) {
    const char* t = "I DSO152 " FW_VER "\n";
    int n = 0; while (t[n]) n++;
    usb_write(t, n);
}
// Lines from the PC: "K x" presses a key (U D L R O M A S), "C x <n>" sets
// a value directly (the PC page's drop-down lists):
//   V V/div index   T time/div index   P position px   L trigger mV (-999999 auto)
//   C DC 1 / AC 0   E rising 1/0       M trigger mode  X probe x10 1/0
//   G generator     U cursors          R run 1 / stop 0  S selected field
//   Q measurement: place * 16 + measurement   I menu item: select it and OK
//   F typed generator frequency, Hz   Y duty %   A averaging 0..3
//   H trigger x, px from the centre
//   Z 1 switch off (as holding the power key)
static char     s_rxl[24];
static uint8_t  s_rxn;
static char     s_cmd_c;
static int32_t  s_cmd_v;
static void pc_keys(KeyEvents* ev) {
    int c;
    while ((c = usb_getc()) >= 0) {
        if (c != '\n' && c != '\r') { if (s_rxn < sizeof s_rxl - 1) s_rxl[s_rxn++] = (char)c; continue; }
        s_rxl[s_rxn] = 0;
        if (s_rxn >= 3 && s_rxl[0] == 'K') {
            static const char KM[] = "UDLROMAS";
            static const uint8_t KK[] = {K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_MODE, K_AUTO, K_RUN};
            for (int i = 0; i < 8; i++)
                if (s_rxl[2] == KM[i]) { if (KK[i] <= K_RIGHT) ev->press |= KB(KK[i]); else ev->shortp |= KB(KK[i]); }
        } else if (s_rxn >= 5 && s_rxl[0] == 'C') {
            const char* q = s_rxl + 4;
            int neg = *q == '-';
            if (neg) q++;
            int32_t v = 0;
            while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
            s_cmd_v = neg ? -v : v;
            s_cmd_c = s_rxl[2];
        }
        s_rxn = 0;
    }
}

// ----------------------------------------------------------------- roll ---
// From 50 ms/div the samples come in continuously (DMA round the buffer)
// and the trace moves right to left, newest on the right, no trigger.
static uint16_t r_buf[NS];
static int      r_n;                    // valid samples, at the right end
static uint16_t r_rd;
static uint8_t  s_roll_on;
static uint32_t s_roll_t;
static void roll_start(void) {
    s_fs = acq_start_circ(s_fs);
    r_rd = 0;
    s_roll_on = 1;
}
static void roll_stop(void) {
    if (s_roll_on) acq_stop();
    s_roll_on = 0;
}
static int roll_take(void) {
    uint16_t w = acq_pos();
    int k = (int)((w + ACQ_MAX - r_rd) % ACQ_MAX);
    if (!k) return 0;
    if (k >= NS) {
        for (int i = 0; i < NS; i++) r_buf[i] = g_acq[(r_rd + k - NS + i) % ACQ_MAX];
    } else {
        for (int i = 0; i < NS - k; i++) r_buf[i] = r_buf[i + k];
        for (int i = 0; i < k; i++) r_buf[NS - k + i] = g_acq[(r_rd + i) % ACQ_MAX];
    }
    r_rd = w;
    r_n += k;
    if (r_n > NS) r_n = NS;
    return k;
}

static int capture_blocking(void) {             // for autoset / zero
    s_fs = acq_start(s_fs, NCAP);
    uint32_t t0 = g_ms;
    while (!acq_done()) { wdg_kick(); if (g_ms - t0 > 2500) { acq_stop(); return 0; } }
    return 1;
}

// ------------------------------------------------------------ autoset ---
// Minimum and maximum over two windows, 20 ms (fast signals) and 400 ms
// (slow ones, down to a few hertz): one short window alone saw only the flat
// top of a 10 Hz square wave and chose a tiny scale.
static int amp_measure(int32_t* mn, int32_t* mx) {
    static const uint8_t TB[] = {5, 9};         // 1 ms/div and 20 ms/div captures
    *mn = 4095; *mx = 0;
    for (unsigned k = 0; k < sizeof TB; k++) {
        s_fs = 30000000u / TDIV_US[TB[k]];
        if (!capture_blocking()) return 0;
        for (int i = 0; i < NCAP; i++) {
            int v = g_acq[i];
            if (v < *mn) *mn = v;
            if (v > *mx) *mx = v;
        }
    }
    return 1;
}

static void autoset(void) {
    toast("Ajuste automático...", C_TRIG);
    cfg.trig_auto = 1;
    cfg.pos = 0;
    cfg.vdiv = NVDIV - 1;               // start from the least sensitive scale
    cfg.tdiv = 5;
    int32_t vpp = 0;
    for (int it = 0; it < 4; it++) {
        apply_scales();
        delay_ms(40);
        int32_t mn, mx;
        if (!amp_measure(&mn, &mx)) break;
        int clipped = mx - zero() > 1450 || zero() - mn > 1450;   // front end saturates ~1500 counts out
        vpp = counts_to_mv(mx - mn);
        int32_t mid = counts_to_mv((mx + mn) / 2 - zero());
        uint8_t v = NVDIV - 1;
        for (uint8_t i = 0; i < NVDIV; i++) if (vpp <= 6 * (int32_t)VDIV_MV[i]) { v = i; break; }
        if (clipped && v < cfg.vdiv) v = cfg.vdiv;          // never zoom in on a cut-off reading
        if (vpp < 30) v = 3;                                // no signal: 100 mV/div
        int32_t pos = -mid * DIVY / (int32_t)VDIV_MV[v];
        if (pos > 3 * DIVY) pos = 3 * DIVY;
        if (pos < -3 * DIVY) pos = -3 * DIVY;
        pos = pos / POS_STEP * POS_STEP;
        int same = (v == cfg.vdiv) && (pos == cfg.pos);
        cfg.vdiv = v; cfg.pos = (int16_t)pos;
        if (same) break;
    }
    // time base: about three periods on the screen. Fastest first, so a
    // fast signal is never read through a slow, aliasing capture.
    cfg.tdiv = 5;
    if (vpp >= 30) {
        static const uint8_t TRY[] = {0, 5, 9};
        for (unsigned k = 0; k < sizeof TRY; k++) {
            cfg.tdiv = TRY[k];
            apply_scales();
            if (!capture_blocking()) break;
            analyse();
            if (m_freq_mhz) {
                uint64_t period_ns = 1000000000000ull / m_freq_mhz;
                uint32_t want_us = (uint32_t)(period_ns * 3 / 10 / 1000);
                uint8_t t = NTDIV - 1;
                for (uint8_t i = 0; i < NTDIV; i++) if (TDIV_US[i] >= want_us) { t = i; break; }
                cfg.tdiv = t;
                break;
            }
            cfg.tdiv = 5;
        }
    }
    apply_scales();
    mark_dirty();
}

// --------------------------------------------------------- zero ranges ---
static void progress(int done, int total) {
    lcd_fill(80, 124, 160, 8, C_BOX);
    lcd_fill(80, 124, 160 * done / total, 8, C_WAVE);
}
static int zero_all(void) {
    int16_t nz[2][FE_RANGES];
    uint8_t dc0 = cfg.dc;
    int ok = 1, n = 0;
    lcd_fill(60, 88, 200, 54, C_BAR);
    lcd_text(160 - lcd_text_w("Medindo o zero das faixas...", 1) / 2, 102,
             "Medindo o zero das faixas...", 1, C_TEXT, C_BAR);
    for (int c = 0; c < 2 && ok; c++) {
        fe_set_dc(c == 0);
        delay_ms(c == 0 ? 50 : 500);                        // AC: let the capacitor settle
        for (uint8_t r = 0; r < FE_RANGES && ok; r++) {
            fe_set_range(r);
            delay_ms(40);
            s_fs = 100000;
            if (!capture_blocking()) { ok = 0; break; }
            int32_t sum = 0, mn = 4095, mx = 0;
            for (int i = 0; i < NCAP; i++) { int v = g_acq[i]; sum += v; if (v < mn) mn = v; if (v > mx) mx = v; }
            int32_t avg = sum / NCAP;
            int32_t ref = (c == 0) ? fz_dc[r] : fz_ac[r];
            if (avg < ref - 120 || avg > ref + 120 || mx - mn > 200) ok = 0;   // input not shorted?
            nz[c][r] = (int16_t)avg;
            progress(++n, 2 * FE_RANGES);
        }
    }
    fe_set_dc(dc0);
    if (ok) {
        for (int r = 0; r < FE_RANGES; r++) { cfg.zero_dc[r] = nz[0][r]; cfg.zero_ac[r] = nz[1][r]; }
        cfg.user_zero = 1;
        ok = settings_save(&cfg);
        s_dirty = 0;
    }
    apply_scales();
    return ok;
}

static void factory_zero(void) {
    for (int r = 0; r < FE_RANGES; r++) { cfg.zero_dc[r] = fz_dc[r]; cfg.zero_ac[r] = fz_ac[r]; }
    cfg.user_zero = 0;
    mark_dirty();
}

// -------------------------------------------------------- diagnostics ---
#define DX 8                    // labels
#define DV 200                  // values
static void diag_value(int y, const char* v, uint16_t c) {
    lcd_fill(DV, y, LCD_W - DV, 8, C_BG);
    lcd_text(DV, y, v, 1, c, C_BG);
}
static void diag_section(int y, const char* t) {
    lcd_text(DX, y, t, 1, C_TIME, C_BG);
    lcd_fill(DX + lcd_text_w(t, 1) + 6, y + 3, LCD_W - 2 * DX - lcd_text_w(t, 1) - 6, 1, C_GRID2);
}
static const char* const MONTH = "JanFebMarAprMayJunJulAugSepOctNovDec";
static void build_date(char* b) {               // __DATE__ "Nov 16 2026" -> "16/11/2026"
    const char* d = __DATE__;
    int m = 0;
    for (int i = 0; i < 12; i++)
        if (MONTH[3 * i] == d[0] && MONTH[3 * i + 1] == d[1] && MONTH[3 * i + 2] == d[2]) m = i + 1;
    b[0] = d[4] == ' ' ? '0' : d[4]; b[1] = d[5]; b[2] = '/';
    b[3] = (char)('0' + m / 10); b[4] = (char)('0' + m % 10); b[5] = '/';
    b[6] = d[7]; b[7] = d[8]; b[8] = d[9]; b[9] = d[10]; b[10] = 0;
}

#define DROW 11
static void draw_diag_static(void) {
    char b[32], *p;
    lcd_fill(0, 0, LCD_W, LCD_H, C_BG);
    lcd_fill(0, 0, LCD_W, TB_H, C_BAR);
    lcd_text(DX, 7, "Diagnóstico", 1, C_TEXT, C_BAR);
    lcd_text(LCD_W - DX - lcd_text_w("Firmware " FW_VER, 1), 7, "Firmware " FW_VER, 1, C_MUTED, C_BAR);

    int y = 28;
    diag_section(y, "Sistema");                               y += DROW + 2;
    lcd_text(DX, y, "Versão do firmware", 1, C_MUTED, C_BG);
    p = b; { const char* t = FW_VER " de "; while (*t) *p++ = *t++; } build_date(p);
    diag_value(y, b, C_TEXT);                                 y += DROW;
    lcd_text(DX, y, "Processador", 1, C_MUTED, C_BG);
    diag_value(y, hw_fast_clock() ? "CH32F103, 72 MHz" : "CH32F103, 8 MHz", C_TEXT);   y += DROW;
    lcd_text(DX, y, "Memória flash", 1, C_MUTED, C_BG);
    p = u2s(*(volatile uint16_t*)0x1FFFF7E0u, b); { const char* t = " KB"; while ((*p++ = *t++)) {} }
    diag_value(y, b, C_TEXT);                                 y += DROW;
    lcd_text(DX, y, "Identificação do chip", 1, C_MUTED, C_BG);
    {
        static const char H[] = "0123456789ABCDEF";
        uint32_t id = ((volatile uint32_t*)0x1FFFF7E8u)[0] ^ ((volatile uint32_t*)0x1FFFF7E8u)[1] ^
                      ((volatile uint32_t*)0x1FFFF7E8u)[2];
        for (int i = 0; i < 8; i++) b[i] = H[(id >> (28 - 4 * i)) & 15];
        b[8] = 0;
    }
    diag_value(y, b, C_TEXT);                                 y += DROW;
    lcd_text(DX, y, "Tempo ligado", 1, C_MUTED, C_BG);        y += DROW;
    lcd_text(DX, y, "Conexão USB", 1, C_MUTED, C_BG);         y += DROW + 6;

    diag_section(y, "Calibração");                            y += DROW + 2;
    lcd_text(DX, y, "Calibração de fábrica", 1, C_MUTED, C_BG);
    diag_value(y, s_factory_ok ? "OK" : "Não encontrada", s_factory_ok ? C_WAVE : C_RED);   y += DROW;
    lcd_text(DX, y, "Zero em uso", 1, C_MUTED, C_BG);
    diag_value(y, cfg.user_zero ? "Ajustado pelo usuário" : "De fábrica", C_TEXT);         y += DROW + 6;

    diag_section(y, "Alimentação");                           y += DROW + 2;
    lcd_text(DX, y, "Bateria", 1, C_MUTED, C_BG);             y += DROW;
    lcd_text(DX, y, "Estado", 1, C_MUTED, C_BG);              y += DROW;
    lcd_text(DX, y, "Divisor da bateria", 1, C_MUTED, C_BG);  y += DROW;
    lcd_text(DX, y, "Tensão interna (VDD)", 1, C_MUTED, C_BG);

    const char* foot = "Pressione qualquer botão para sair";
    lcd_fill(0, LCD_H - 18, LCD_W, 18, C_BAR);
    lcd_text((LCD_W - lcd_text_w(foot, 1)) / 2, LCD_H - 13, foot, 1, C_MUTED, C_BAR);
}
static void draw_diag_values(void) {
    char b[24], *p;
    // uptime (the y positions follow draw_diag_static)
    uint32_t t = g_ms / 1000;
    p = b;
    *p++ = (char)('0' + t / 36000 % 10); *p++ = (char)('0' + t / 3600 % 10); *p++ = ':';
    *p++ = (char)('0' + t / 600 % 6);    *p++ = (char)('0' + t / 60 % 10);   *p++ = ':';
    *p++ = (char)('0' + t / 10 % 6);     *p++ = (char)('0' + t % 10);        *p = 0;
    diag_value(28 + DROW + 2 + 4 * DROW, b, C_TEXT);
    {
        static const char* const U[] = {"Sem conexão", "PC conectado, porta fechada", "Transmitindo"};
        int u = usb_state();
        diag_value(28 + DROW + 2 + 5 * DROW, U[u], u == 2 ? C_WAVE : u ? C_TRIG : C_MUTED);
    }

    int y = 28 + DROW + 2 + 6 * DROW + 6 + DROW + 2 + 2 * DROW + 6 + DROW + 2;   // "Bateria"
    s_bat_t = 0;
    battery_poll();
    p = fmt_mv(s_bat_mv, b); *p++ = ' '; *p++ = '('; p = u2s((uint32_t)batt_percent(), p); *p++ = '%'; *p++ = ')'; *p = 0;
    diag_value(y, b, C_TEXT);                                 y += DROW;
    diag_value(y, s_bat_chg ? "Carregando" : "Sem carregador", s_bat_chg ? C_TRIG : C_TEXT);   y += DROW;
    p = u2s(bat_ratio() / 1000, b); *p++ = '.'; p = frac(bat_ratio() % 1000, 3, p);
    { const char* q = cfg.bat_ratio ? " (calibrado)" : " (padrão)"; while ((*p++ = *q++)) {} }
    diag_value(y, b, C_TEXT);                                 y += DROW;
    uint16_t vref = acq_read_channel(17);
    fmt_mv(vref ? (int32_t)(1200u * 4095u / vref) : 0, b);
    diag_value(y, b, C_TEXT);
    fe_set_range(range_for());
}

// --------------------------------------------------------------- about ---
static void draw_about(void) {
    char b[32], *p;
    lcd_fill(0, 0, LCD_W, LCD_H, C_BG);
    lcd_fill(0, 0, LCD_W, TB_H, C_BAR);
    lcd_text(DX, 7, "Sobre", 1, C_TEXT, C_BAR);
    lcd_text(LCD_W - DX - lcd_text_w("Firmware " FW_VER, 1), 7, "Firmware " FW_VER, 1, C_MUTED, C_BAR);
    int y = 30;
    lcd_text(DX, y, "DSO152 - firmware alternativo", 1, C_WAVE, C_BG);                 y += 16;
    lcd_text(DX, y, "Versão", 1, C_MUTED, C_BG);
    p = put(b, FW_VER " de "); build_date(p);
    diag_value(y, b, C_TEXT);                                                           y += DROW;
    lcd_text(DX, y, "Autor", 1, C_MUTED, C_BG);
    diag_value(y, FW_AUTHOR, C_TEXT);                                                   y += DROW;
    lcd_text(DX, y, "Licença", 1, C_MUTED, C_BG);
    diag_value(y, "MIT (código aberto)", C_TEXT);                                       y += DROW;
    lcd_text(DX, y, "Aparelho", 1, C_MUTED, C_BG);
    diag_value(y, "FNIRSI DSO-152", C_TEXT);                                            y += DROW + 6;

    diag_section(y, "Novidades desta versão");                                          y += DROW + 3;
    static const char* const NEWS[] = {
        "Multímetro com escala automática ou fixa",
        "Média de capturas e posição do disparo",
        "Tempo de subida/descida; aviso de tensão perigosa",
        "Gerador: frequência livre (PC) e duty ajustável"};
    for (unsigned i = 0; i < sizeof NEWS / sizeof NEWS[0]; i++) {
        lcd_fill(DX + 2, y + 3, 2, 2, C_WAVE);
        lcd_text(DX + 10, y, NEWS[i], 1, C_TEXT, C_BG);                                y += DROW;
    }
    y += 6;
    lcd_text(DX, y, "O bootloader FNIRSI não é alterado: para voltar", 1, C_MUTED, C_BG);  y += DROW;
    lcd_text(DX, y, "ao original, instale o arquivo da FNIRSI.", 1, C_MUTED, C_BG);

    const char* foot = "Pressione qualquer botão para sair";
    lcd_fill(0, LCD_H - 18, LCD_W, 18, C_BAR);
    lcd_text((LCD_W - lcd_text_w(foot, 1)) / 2, LCD_H - 13, foot, 1, C_MUTED, C_BAR);
}

// ---------------------------------------------------------- multimeter ---
// Voltmeter page: every value at once, one of them big. The range follows
// the signal (AUTO) or stays where the user put it; the sampling rate
// follows the frequency so that slow and fast signals both read right.
static const uint8_t DMM_R[] = {0, 1, 2, 4, 5, 7, 8};     // front-end ranges used
// The input stage saturates before the ADC does: about 1740 counts from zero
// were seen on range 0 (a 1.5 V cell read as 174 mV). The multimeter only
// trusts 1400 counts, goes up a range above that and down below 900.
#define DMM_FULL 1400
#define DMM_LOW  900
#define NDMM_R (sizeof DMM_R / sizeof DMM_R[0])
static const uint32_t DMM_FS[] = {6000, 60000, 600000, 2400000};
static const char* const DMM_LABEL[6] = {"VDC", "VRMS", "FREQ", "VPP", "VMAX", "VMIN"};
static uint8_t d_r, d_fsi, d_hold, d_skip, d_clip;
static int64_t d_uv[6];                 // VDC VRMS - VPP VMAX VMIN, microvolts at the tip
static uint8_t d_alert;

static int64_t uv_of(int32_t f16) {
    return (int64_t)(f16 - zero() * 16) * 1000000 / (cpv() * 16) * pf();
}
static LO char* fmt_uv(int64_t uv, char* p) {      // four significant digits
    if (uv < 0) { *p++ = '-'; uv = -uv; }
    uint32_t a = uv > 999999999 ? 999999999u : (uint32_t)uv, v;
    const char* unit = "V";
    if (a >= 1000000) v = a / 1000;                 // in mV, shown as V
    else { v = a; unit = "mV"; }                    // in uV, shown as mV
    p = u2s(v / 1000, p); *p++ = '.';
    uint32_t r = v % 1000;
    if (v < 10000) p = frac(r, 3, p);
    else if (v < 100000) p = frac(r / 10, 2, p);
    else p = frac(r / 100, 1, p);
    if (a < 1000000 && v < 10000) p[-1] = 0, p--;   // x.xx mV: noise below that
    return put(p, unit);
}
static LO void dmm_value_text(int k, char* b) {
    if (k == 2) { fmt_freq(m_freq_mhz, b); return; }
    if (d_clip && cfg.dmm_range) { put(b, "----"); return; }
    fmt_uv(d_uv[k], b);
}
static LO void dmm_range_text(char* b) {
    char* p = put(b, cfg.dmm_range ? "FIXA " : "AUTO ");
    fmt_mv((int32_t)(DMM_FULL * 1000u / fe_counts_per_volt(DMM_R[d_r])) * pf(), p);
}
static LO void dmm_static(void) {
    char b[24];
    lcd_fill(0, 0, LCD_W, LCD_H, C_BG);
    lcd_fill(0, 0, LCD_W, TB_H, C_BAR);
    lcd_text(6, 7, "Multímetro", 1, C_TEXT, C_BAR);
    lcd_text(76, 7, cfg.dc ? "DC" : "AC", 1, C_TEXT, C_BAR);
    dmm_range_text(b);
    lcd_text(98, 7, b, 1, cfg.dmm_range ? C_TRIG : C_TIME, C_BAR);
    lcd_text(196, 7, cfg.probe10 ? "PONTA x10" : "PONTA x1", 1, cfg.probe10 ? C_TRIG : C_MUTED, C_BAR);
    icon_battery(294, 5, batt_level(), s_bat_chg, C_BAR);
    if (d_hold) { lcd_fill(250, 4, 36, TB_H - 8, C_RED); lcd_text(256, 7, "HOLD", 1, C_TEXT, C_RED); }
    lcd_text(12, 50, DMM_LABEL[cfg.dmm_focus], 2, C_MUTED, C_BG);
    for (int k = 0, j = 0; k < 6; k++) {
        if (k == cfg.dmm_focus) continue;
        int x = 8 + (j % 3) * 104, y = 138 + (j / 3) * 34;
        lcd_text(x, y, DMM_LABEL[k], 1, C_MUTED, C_BG);
        j++;
    }
    const char* k = "Setas: medida/ponta RUN:hold AUTO:escala OK:sair";
    lcd_fill(0, LCD_H - 18, LCD_W, 18, C_BAR);
    lcd_text((LCD_W - lcd_text_w(k, 1)) / 2, LCD_H - 13, k, 1, C_MUTED, C_BAR);
}
static LO void dmm_values(void) {
    char b[24];
    int al = d_alert;
    if (al) { lcd_fill(0, 26, LCD_W, 16, C_RED); lcd_text(8, 30, ALERT_TXT[al], 1, C_TEXT, C_RED); }
    else lcd_fill(0, 26, LCD_W, 16, C_BG);
    dmm_value_text(cfg.dmm_focus, b);
    int e = lcd_text(12, 76, b, 4, al ? C_RED : C_TEXT, C_BG);
    lcd_fill(e, 76, LCD_W - e, 28, C_BG);
    if (d_clip && cfg.dmm_range) lcd_text(12, 110, "FORA DA ESCALA", 1, C_RED, C_BG);
    else lcd_fill(12, 110, 100, 8, C_BG);
    for (int k = 0, j = 0; k < 6; k++) {
        if (k == cfg.dmm_focus) continue;
        int x = 8 + (j % 3) * 104, y = 150 + (j / 3) * 34;
        dmm_value_text(k, b);
        e = lcd_text(x, y, b, 2, C_TEXT, C_BG);
        lcd_fill(e, y, x + 102 - e, 14, C_BG);
        j++;
    }
}
static LO void pc_dmm(void) {
    if (usb_state() != 2) return;
    char b[160], *p = b;
    *p++ = 'V'; *p++ = ' ';
    for (int k = 0; k < 6; k++) {
        if (k == 2) { p = put_u(p, m_freq_mhz); continue; }
        int64_t v = d_uv[k];
        if (v > 2000000000) v = 2000000000;
        if (v < -2000000000) v = -2000000000;
        p = put_i(p, (int32_t)v);
    }
    p = put_u(p, cfg.dmm_focus); p = put_u(p, d_hold); p = put_u(p, cfg.dmm_range);
    p = put_u(p, DMM_FULL * 1000u / fe_counts_per_volt(DMM_R[d_r]) * (uint32_t)pf());
    p = put_u(p, (uint32_t)pf()); p = put_u(p, cfg.dc); p = put_u(p, d_alert); p = put_u(p, d_clip);
    p[-1] = '\n';
    usb_write(b, (int)(p - b));
}
static void dmm_range_set(int r) {
    d_r = (uint8_t)r;
    fe_set_range(DMM_R[d_r]);
    f_valid = 0;
    d_skip = 2;                         // the relay and the input settle
}
static LO void dmm_enter(void) {
    d_fsi = 0; d_hold = 0; d_clip = 0;
    if (cfg.dmm_focus > 5) cfg.dmm_focus = 0;
    if (cfg.dmm_range > NDMM_R) cfg.dmm_range = 0;
    dmm_range_set(cfg.dmm_range ? cfg.dmm_range - 1 : 3);
    for (int k = 0; k < 6; k++) d_uv[k] = 0;
    m_freq_mhz = 0;
    d_alert = 0;
    dmm_static();
    dmm_values();
}
// One capture done: measure, change range / rate if needed. 1 = new values.
static LO int dmm_capture(void) {
    if (d_skip) { d_skip--; return 0; }
    stats(g_acq, NCAP);
    int32_t z = zero();
    int32_t pk = m_max - z > z - m_min ? m_max - z : z - m_min;
    d_clip = (uint8_t)(pk > DMM_FULL || m_max >= 4090 || m_min <= 5);
    if (!cfg.dmm_range) {
        if (d_clip && d_r + 1 < (int)NDMM_R) { dmm_range_set(d_r + 1); return 0; }
        if (d_r > 0 && (int64_t)pk * fe_counts_per_volt(DMM_R[d_r - 1]) <
                       (int64_t)DMM_LOW * fe_counts_per_volt(DMM_R[d_r])) {
            dmm_range_set(d_r - 1); return 0;
        }
    }
    // rate: at least 20 samples a period, at least 3 periods in the capture
    if (m_freq_mhz) {
        uint64_t per = (uint64_t)DMM_FS[d_fsi] * 1000u / m_freq_mhz;      // samples a period
        if (per < 20 && d_fsi + 1 < 4) { d_fsi++; f_valid = 0; d_skip = 1; return 0; }
        if (per * 3 > NCAP && d_fsi > 0) { d_fsi--; f_valid = 0; d_skip = 1; return 0; }
    } else if (d_fsi > 0 && m_max - m_min < 24) { d_fsi = 0; f_valid = 0; d_skip = 1; return 0; }
    smooth();
    d_uv[0] = uv_of(f_avg);
    d_uv[1] = (int64_t)f_rms * 1000000 / (cpv() * 16) * pf();
    d_uv[4] = uv_of(f_max);
    d_uv[5] = uv_of(f_min);
    d_uv[3] = d_uv[4] - d_uv[5];
    d_alert = (uint8_t)alert_of((int32_t)(d_uv[0] / 1000), (int32_t)(d_uv[1] / 1000),
                                (int32_t)(d_uv[4] / 1000), (int32_t)(d_uv[5] / 1000));
    return 1;
}

// --------------------------------------------------------------- menu ---
// Four lists: the main menu, "Medidas na tela", "Sistema" and "Gerador de sinal".
enum { I_DMM, I_TRIG, I_EDGE, I_PROBE, I_CUR, I_AVG, I_GENM, I_MEAS, I_SYS, I_CLOSE,
       I_M1, I_M2, I_M3, I_M4, I_M5, I_MDEF,
       I_ZERO, I_FACTORY, I_BRIGHT, I_AUTOOFF, I_BATT, I_DIAG, I_ABOUT,
       I_GEN, I_DUTY, I_BACK, NITEM };
static const char* const ITEM_NAME[NITEM] = {
    "Multímetro", "Modo do trigger", "Borda do trigger", "Ponta de prova", "Cursores",
    "Média de capturas", "Gerador de sinal", "Medidas na tela", "Sistema", "Fechar",
    "Posição 1", "Posição 2", "Posição 3", "Posição 4", "Posição 5", "Restaurar padrão",
    "Ajustar zero (todas as faixas)", "Restaurar zero de fábrica",
    "Brilho da tela", "Desligamento automático", "Calibrar bateria", "Diagnóstico", "Sobre",
    "Frequência", "Duty cycle", "Voltar"};
static const uint8_t MN_MAIN[] = {I_DMM, I_TRIG, I_EDGE, I_PROBE, I_CUR, I_AVG, I_GENM, I_MEAS, I_SYS, I_CLOSE};
static const uint8_t MN_MEASL[] = {I_M1, I_M2, I_M3, I_M4, I_M5, I_MDEF, I_BACK};
static const uint8_t MN_SYSL[] = {I_ZERO, I_FACTORY, I_BRIGHT, I_AUTOOFF, I_BATT, I_DIAG, I_ABOUT, I_BACK};
static const uint8_t MN_GENL[] = {I_GEN, I_DUTY, I_BACK};
static const uint8_t* const MN_ITEMS[4] = {MN_MAIN, MN_MEASL, MN_SYSL, MN_GENL};
static const uint8_t MN_N[4] = {sizeof MN_MAIN, sizeof MN_MEASL, sizeof MN_SYSL, sizeof MN_GENL};
static const uint8_t MN_PARENT[4] = {I_CLOSE, I_MEAS, I_SYS, I_GENM};
static const char* const MN_TITLE[4] = {"Configurações", "Medidas na tela", "Sistema", "Gerador de sinal"};
static const char* const AVG_TXT[4] = {"Desligada", "4", "8", "16"};
static char* gen_text(char* b) {
    uint32_t hz = gen_hz_now();
    if (!hz) return put(b, "Desligado");
    char* p = hz >= 1000 && hz % 1000 == 0 ? u2s(hz / 1000, b) : u2s(hz, b);
    return put(p, hz >= 1000 && hz % 1000 == 0 ? " kHz" : " Hz");
}
static const char* const BRIGHT_TXT[4] = {"100%", "75%", "50%", "25%"};
static const uint8_t BRIGHT_PCT[4] = {100, 75, 50, 25};
static const char* const AUTOOFF_TXT[4] = {"Desligado", "5 min", "10 min", "30 min"};
static const uint8_t AUTOOFF_MIN[4] = {0, 5, 10, 30};
static uint8_t s_menu, s_mi;
#define MX 40
#define MY 23
#define MW 240
#define MROW 16

static const char* item_value(int id) {
    static char vb[16];
    switch (id) {
    case I_GEN: gen_text(vb); return vb;
    case I_GENM: put(gen_text(vb), " >"); return vb;
    case I_DUTY: { char* q = u2s((uint32_t)gen_duty_now(), vb); put(q, "%"); return vb; }
    case I_AVG:   return AVG_TXT[cfg.avg & 3];
    case I_TRIG:  return cfg.trig_mode == TM_NORMAL ? "Normal" : cfg.trig_mode == TM_SINGLE ? "Single" : "Auto";
    case I_EDGE:  return cfg.rising ? "Subida" : "Descida";
    case I_PROBE: return cfg.probe10 ? "x10" : "x1";
    case I_CUR:   return cfg.cursors == CUR_TIME ? "Tempo" : cfg.cursors == CUR_VOLT ? "Tensão" : "Desligado";
    case I_MEAS: case I_SYS: return ">";
    case I_DMM: return "Abrir";
    case I_M1: case I_M2: case I_M3: case I_M4: case I_M5: return MS_NAME[meas_id(id - I_M1)];
    case I_BRIGHT:  return BRIGHT_TXT[cfg.bright & 3];
    case I_AUTOOFF: return AUTOOFF_TXT[cfg.auto_off & 3];
    default: return "";
    }
}
static void pc_menu(void) {                     // the open menu, for the PC's screen
    if (usb_state() != 2) return;
    char* p = s_pcbuf;
    p = put(p, "U 1 "); p = u2s(s_mi, p);
    *p++ = '\t'; p = put(p, MN_TITLE[s_menu]);
    for (int i = 0; i < MN_N[s_menu]; i++) {
        int id = MN_ITEMS[s_menu][i];
        *p++ = '\t'; p = put(p, ITEM_NAME[id]);
        *p++ = '\t'; p = put(p, item_value(id));
    }
    *p++ = '\n';
    usb_write(s_pcbuf, (int)(p - s_pcbuf));
}
static void draw_menu(void) {
    int n = MN_N[s_menu];
    int h = n * MROW + 42;
    lcd_fill(MX - 1, MY - 1, MW + 2, h + 2, C_GRID2);
    lcd_fill(MX, MY, MW, h, C_BAR);
    lcd_text(MX + 12, MY + 9, MN_TITLE[s_menu], 1, C_MUTED, C_BAR);
    for (int i = 0; i < n; i++) {
        int id = MN_ITEMS[s_menu][i];
        int y = MY + 26 + i * MROW;
        uint16_t bg = (i == s_mi) ? C_BOX : C_BAR;
        lcd_fill(MX + 4, y, MW - 8, MROW - 2, bg);
        if (i == s_mi) lcd_fill(MX + 4, y, 3, MROW - 2, C_WAVE);
        lcd_text(MX + 14, y + 4, ITEM_NAME[id], 1, C_TEXT, bg);
        const char* v = item_value(id);
        lcd_text(MX + MW - 12 - lcd_text_w(v, 1), y + 4, v, 1, C_TRIG, bg);
    }
    const char* k = "Setas: escolher   OK: alterar";
    lcd_text(MX + (MW - lcd_text_w(k, 1)) / 2, MY + h - 13, k, 1, C_MUTED, C_BAR);
    pc_menu();
}

// --------------------------------------------------- battery calibration ---
#define BD_X 24
#define BD_Y 46
#define BD_W 272
#define BD_H 150
static void batt_dialog(void) {
    lcd_fill(BD_X - 1, BD_Y - 1, BD_W + 2, BD_H + 2, C_GRID2);
    lcd_fill(BD_X, BD_Y, BD_W, BD_H, C_BAR);
    int x = BD_X + 12, y = BD_Y + 10;
    lcd_text(x, y, "Calibrar bateria", 1, C_TRIG, C_BAR);                      y += 18;
    lcd_text(x, y, "Use com a bateria totalmente carregada:", 1, C_TEXT, C_BAR); y += 11;
    lcd_text(x, y, "carregue até o fim e retire o", 1, C_TEXT, C_BAR);         y += 11;
    lcd_text(x, y, "carregador. A leitura atual passa a", 1, C_TEXT, C_BAR);    y += 11;
    lcd_text(x, y, "valer 4,20 V (100%).", 1, C_TEXT, C_BAR);                  y += 18;
    lcd_text(x, y, "Leitura atual", 1, C_MUTED, C_BAR);
    lcd_text(x, y + 12, "Divisor", 1, C_MUTED, C_BAR);
    lcd_text(x, BD_Y + BD_H - 26, "OK: calibrar   MODE: valor padrão", 1, C_MUTED, C_BAR);
    lcd_text(x, BD_Y + BD_H - 14, "Outro botão: cancelar", 1, C_MUTED, C_BAR);
}
static void batt_dialog_values(void) {
    char b[24], *p;
    int x = BD_X + 120, y = BD_Y + 10 + 18 + 33 + 18;
    s_bat_t = 0;
    battery_poll();
    p = fmt_mv(s_bat_mv, b); p = put(p, s_bat_chg ? " (carregando)" : "              ");
    lcd_text(x, y, b, 1, s_bat_chg ? C_TRIG : C_TEXT, C_BAR);
    p = u2s(bat_ratio() / 1000, b); *p++ = '.'; p = frac(bat_ratio() % 1000, 3, p);
    put(p, cfg.bat_ratio ? " (calibrado)" : " (padrão)   ");
    lcd_text(x, y + 12, b, 1, C_TEXT, C_BAR);
}

// --------------------------------------------------------------- main ---
static void redraw_all(void) {
    lcd_fill(0, 0, LCD_W, LCD_H, C_BG);
    draw_top();
    draw_grid();
    lcd_fill(0, GY + GH + 2, LCD_W, LCD_H - (GY + GH + 2), C_BAR);
    if (f_valid) draw_bottom();
}

static void load_config(void) {
    const volatile uint16_t* z = (const volatile uint16_t*)0x0800FE00u;
    s_factory_ok = *(const volatile uint8_t*)0x0800FE3Eu == 0x15 &&
                   *(const volatile uint8_t*)0x0800FE50u == 0x41;
    for (int r = 0; r < FE_RANGES; r++) {
        uint16_t dc = z[r], ac = z[FE_RANGES + r];
        if (!(dc > 1500 && dc < 2600 && ac > 1500 && ac < 2600)) s_factory_ok = 0;
        fz_dc[r] = (int16_t)dc; fz_ac[r] = (int16_t)ac;
    }
    if (!s_factory_ok) for (int r = 0; r < FE_RANGES; r++) fz_dc[r] = fz_ac[r] = 2048;

    int loaded = settings_load(&cfg);
    if (loaded == 4 || loaded == 5) {               // v2.0 steps: off 10 100 1k 10k 100k
        static const uint8_t OLD_GEN[6] = {0, 1, 4, 6, 8, 10};
        cfg.gen = cfg.gen < 6 ? OLD_GEN[cfg.gen] : GEN_DEFAULT;
    }
    if (!loaded || cfg.vdiv >= NVDIV || cfg.tdiv >= NTDIV || cfg.trig_mode > TM_SINGLE ||
        cfg.pos > POS_MAX || cfg.pos < -POS_MAX) {
        cfg = (Settings){0};
        cfg.vdiv = 6; cfg.tdiv = 4; cfg.dc = 1; cfg.rising = 1; cfg.trig_auto = 1;
        cfg.trig_mode = TM_AUTO;
        cfg.gen = GEN_DEFAULT;
    }
    if (cfg.gen > GEN_CUSTOM || (cfg.gen == GEN_CUSTOM && (cfg.gen_hz < GEN_MIN_HZ || cfg.gen_hz > GEN_MAX_HZ)))
        cfg.gen = GEN_DEFAULT;
    if (cfg.hpos > HPOS_MAX || cfg.hpos < -HPOS_MAX) cfg.hpos = 0;
    cfg.avg &= 3;
    if (cfg.dmm_focus > 5) cfg.dmm_focus = 0;
    if (cfg.cursors > CUR_VOLT) cfg.cursors = CUR_OFF;
    if (!cfg.user_zero) factory_zero();
    s_dirty = 0;
    if (loaded == 4 || loaded == 5) mark_dirty();   // write it again in the new layout
}

int main(void) {
    hw_init();
    keys_init();
    lcd_init();
    fe_init();
    acq_init();
    usb_init();
    load_config();
    fe_set_dc(cfg.dc);
    gen_apply();
    backlight_set(BRIGHT_PCT[cfg.bright & 3]);
    pin_mode(GPIOA, 7, 0);                      // PA7: battery divider, analogue
    battery_poll();
    apply_scales();
    m_trig = zero();
    redraw_all();

    enum { PG_SCOPE, PG_MENU, PG_DIAG, PG_CONFIRM, PG_BATT, PG_MSG, PG_ABOUT, PG_DMM } page = PG_SCOPE;
    uint8_t pc_was = 0;
    uint32_t s_rep_t = 0;
    int s_rep_n = 0;
    uint32_t t_bottom = 0, power_t0 = 0, t_diag = 0;
    int capturing = 0;
    KeyEvents ev;

    for (;;) {
        wdg_kick();
        keys_poll(&ev);
        pc_keys(&ev);
        if (s_cmd_c == 'Z' && s_cmd_v == 1) {          // "desligar" on the PC page
            acq_stop(); roll_stop();
            if (s_dirty) settings_save(&cfg);
            toast("Desligado pelo PC", C_TRIG);
            delay_ms(600);
            power_off();
        }
        {
            int u = usb_state();
            static uint32_t t_st, t_fr;
            if (u == 2 && pc_was != 2) { pc_hello(); t_fr = 0; }
            if (u == 2 && g_ms - t_st > 250) { t_st = g_ms; pc_status(); }
            if (u == 2 && !s_run && s_last_p && g_ms - t_fr > 500) {     // stopped: keep the PC's picture
                t_fr = g_ms; pc_frame(s_last_p, s_last_n); pc_meas();
            }
            pc_was = (uint8_t)u;
            if (page == PG_SCOPE && u != s_pc_drawn) { s_pc_drawn = (uint8_t)u; icon_pc(272, 5, u, C_BAR); }
        }

        // ---- power: hold 2 s
        if (ev.held & KB(K_POWER)) {
            if (!power_t0) power_t0 = g_ms | 1;
            else if ((int32_t)(g_ms - power_t0) > 2000) {
                acq_stop();
                roll_stop();
                if (s_dirty) settings_save(&cfg);
                toast("Desligando...", C_TRIG);
                delay_ms(300);
                power_off();
            }
        } else power_t0 = 0;

        // ---- save 2 s after the last change
        if (s_dirty && g_ms - s_dirty_t > 2000 && page == PG_SCOPE) {
            settings_save(&cfg);
            s_dirty = 0;
        }

        uint16_t any = ev.press | ev.shortp;

        // ---- automatic power-off: only on battery, never with a PC or charger
        {
            static uint32_t s_act;
            static int s_warn = -1;
            uint32_t lim = AUTOOFF_MIN[cfg.auto_off & 3] * 60000u;
            if (any | ev.held | !s_act) {
                s_act = g_ms | 1;
                if (s_warn >= 0) { s_warn = -1; if (page == PG_SCOPE) redraw_all(); continue; }   // the key only wakes
            }
            if (!lim || usb_state() || s_bat_chg) s_act = g_ms | 1;
            else {
                // s_act is g_ms | 1 (never 0), so it can be 1 ms ahead of g_ms: that
                // must count as 0, not as 49 days of inactivity
                uint32_t idle = (int32_t)(g_ms - s_act) > 0 ? g_ms - s_act : 0;
                if (idle >= lim) {
                    acq_stop(); roll_stop();
                    if (s_dirty) settings_save(&cfg);
                    toast("Desligado por inatividade", C_TRIG);
                    delay_ms(800);
                    power_off();
                }
                if (idle + 30000u >= lim && page == PG_SCOPE) {
                    int left = (int)((lim - idle + 999) / 1000);
                    if (left != s_warn) {
                        char b[40], *p = put(b, "Desligando em ");
                        p = u2s((uint32_t)left, p); put(p, " s - aperte um botão");
                        toast(b, C_TRIG);
                        s_warn = left;
                    }
                }
            }
        }

        // ---- what is open, for the PC's screen
        {
            static int8_t ui_sent = -1;
            static uint32_t t_ui;
            int ui = page == PG_SCOPE ? 0 : page == PG_MENU ? 1 : page == PG_DMM ? 3 : 2;
            if (usb_state() != 2) ui_sent = -1;
            else if (ui != ui_sent || g_ms - t_ui > 1000) {
                t_ui = g_ms;
                ui_sent = (int8_t)ui;
                if (ui == 1) pc_menu();
                else {
                    char b[40], *p = put(b, ui == 3 ? "U 3" : ui ? "U 2 " : "U 0");
                    if (ui == 2) p = put(p, page == PG_DIAG ? "Diagnóstico" : page == PG_BATT ? "Calibrar bateria" :
                                       page == PG_ABOUT ? "Sobre" : "Ajuste de zero");
                    *p++ = '\n';
                    usb_write(b, (int)(p - b));
                }
            }
        }

        if (page == PG_DMM) {
            if (s_cmd_c == 'X') { cfg.probe10 = s_cmd_v ? 1 : 0; mark_dirty(); dmm_static(); dmm_values(); }
            s_cmd_c = 0;
            if (ev.shortp & KB(K_OK)) {
                acq_stop(); capturing = 0; a_n = 0;
                page = PG_SCOPE; apply_scales(); redraw_all();
                continue;
            }
            int redo = 0;
            if (ev.press & (KB(K_UP) | KB(K_DOWN))) {
                cfg.dmm_focus = (uint8_t)((cfg.dmm_focus + ((ev.press & KB(K_UP)) ? 5 : 1)) % 6);
                mark_dirty(); redo = 1;
            }
            if (ev.press & (KB(K_LEFT) | KB(K_RIGHT))) { cfg.probe10 = !cfg.probe10; mark_dirty(); redo = 1; }
            if (ev.shortp & KB(K_MODE)) {
                cfg.dc = !cfg.dc; fe_set_dc(cfg.dc); f_valid = 0; d_skip = 2; mark_dirty(); redo = 1;
            }
            if (ev.shortp & KB(K_RUN)) { d_hold = !d_hold; redo = 1; }
            if (ev.shortp & KB(K_AUTO)) {                // AUTO -> fixed here -> next ranges -> AUTO
                if (!cfg.dmm_range) cfg.dmm_range = (uint8_t)(d_r + 1);
                else if (cfg.dmm_range < NDMM_R) { cfg.dmm_range++; dmm_range_set(cfg.dmm_range - 1); }
                else cfg.dmm_range = 0;
                mark_dirty(); redo = 1;
            }
            if (redo) { dmm_static(); dmm_values(); pc_dmm(); }
            if (d_hold) { battery_tick(); continue; }
            if (!capturing) { s_fs = acq_start(DMM_FS[d_fsi], NCAP); capturing = 1; }
            if (!acq_done()) continue;
            capturing = 0;
            battery_tick();
            if (dmm_capture() && g_ms - t_bottom > 250) { t_bottom = g_ms; dmm_values(); pc_dmm(); }
            continue;
        }

        if (page == PG_DIAG) {
            if (any & ~KB(K_POWER)) { page = PG_SCOPE; capturing = 0; gen_apply(); redraw_all(); continue; }
            if (g_ms - t_diag > 700) { t_diag = g_ms; draw_diag_values(); }
            continue;
        }

        if (page == PG_BATT) {
            if (ev.shortp & KB(K_OK)) {
                s_bat_t = 0; battery_poll();
                uint32_t r = s_bat_pin ? 4200000u / s_bat_pin : 0;
                if (r >= 1500 && r <= 3000) {
                    cfg.bat_ratio = (uint16_t)r; mark_dirty(); s_bat_prev = -1; battery_poll();
                    toast("Bateria calibrada: 4,20 V", C_WAVE);
                } else toast("Leitura da bateria inválida", C_RED);
                delay_ms(1200);
                page = PG_SCOPE; redraw_all();
            } else if (ev.shortp & KB(K_MODE)) {
                cfg.bat_ratio = 0; mark_dirty(); s_bat_prev = -1; battery_poll();
                toast("Divisor padrão restaurado", C_WAVE);
                delay_ms(1000);
                page = PG_SCOPE; redraw_all();
            } else if (any & ~KB(K_POWER)) { page = PG_SCOPE; redraw_all(); }
            else if (g_ms - t_diag > 700) { t_diag = g_ms; batt_dialog_values(); }
            continue;
        }

        if (page == PG_CONFIRM) {
            if (ev.shortp & KB(K_OK)) {
                acq_stop(); roll_stop(); capturing = 0;
                int ok = zero_all();
                // result stays on screen until a key is pressed
                lcd_fill(30, 70, 260, 84, C_GRID2);
                lcd_fill(31, 71, 258, 82, C_BAR);
                lcd_text(44, 80, ok ? "Zero ajustado e salvo" : "Ajuste de zero cancelado", 1, ok ? C_WAVE : C_RED, C_BAR);
                lcd_text(44, 100, ok ? "As 22 faixas (DC e AC) foram" : "Há sinal na entrada. Ligue a", 1, C_TEXT, C_BAR);
                lcd_text(44, 112, ok ? "medidas e gravadas." : "ponta ao jacaré e tente de novo.", 1, C_TEXT, C_BAR);
                lcd_text(44, 134, "Pressione qualquer botão", 1, C_MUTED, C_BAR);
                page = PG_MSG;
            } else if (any & ~KB(K_POWER)) { page = PG_SCOPE; redraw_all(); }
            continue;
        }
        if (page == PG_ABOUT) {
            if (any & ~KB(K_POWER)) { page = PG_SCOPE; redraw_all(); }
            continue;
        }
        if (page == PG_MSG) {
            if (any & ~KB(K_POWER)) { page = PG_SCOPE; redraw_all(); }
            continue;
        }

        if (page == PG_MENU) {
            int n = MN_N[s_menu];
            if (s_cmd_c == 'I') {                       // an item clicked on the PC: select and OK
                if (s_cmd_v >= 0 && s_cmd_v < n) { s_mi = (uint8_t)s_cmd_v; ev.shortp |= KB(K_OK); }
                s_cmd_c = 0;
            }
            if (ev.press & (KB(K_UP) | KB(K_LEFT)))    { s_mi = (uint8_t)((s_mi + n - 1) % n); draw_menu(); continue; }
            if (ev.press & (KB(K_DOWN) | KB(K_RIGHT))) { s_mi = (uint8_t)((s_mi + 1) % n); draw_menu(); continue; }
            if (ev.shortp & KB(K_OK)) {
                int id = MN_ITEMS[s_menu][s_mi], changed = 1;
                switch (id) {                           // items with a value: OK steps it
                case I_TRIG:  cfg.trig_mode = (uint8_t)((cfg.trig_mode + 1) % 3); s_run = 1; break;
                case I_EDGE:  cfg.rising = !cfg.rising; break;
                case I_PROBE: cfg.probe10 = !cfg.probe10; break;
                case I_GEN:
                    if (cfg.gen >= GEN_CUSTOM) cfg.gen = 0;
                    else if (cfg.gen + 1 < (int)NGEN) cfg.gen++;
                    else cfg.gen = cfg.gen_hz >= GEN_MIN_HZ && cfg.gen_hz <= GEN_MAX_HZ ? GEN_CUSTOM : 0;
                    gen_apply();
                    break;
                case I_DUTY: {
                    int d0 = gen_duty_now(), nd = DUTY_STEPS[0];
                    for (unsigned k = 0; k < sizeof DUTY_STEPS; k++) if (DUTY_STEPS[k] > d0) { nd = DUTY_STEPS[k]; break; }
                    cfg.gen_duty = (uint8_t)nd; gen_apply();
                    break; }
                case I_AVG:   cfg.avg = (uint8_t)((cfg.avg + 1) & 3); a_n = 0; break;
                case I_CUR:
                    cfg.cursors = (uint8_t)((cfg.cursors + 1) % 3);
                    if (s_sel >= nsel()) s_sel = P_VDIV;
                    break;
                case I_M1: case I_M2: case I_M3: case I_M4: case I_M5: {
                    int k = id - I_M1;
                    cfg.meas[k] = (uint8_t)(meas_id(k) % (NMS - 1) + 1);
                    break; }
                case I_MDEF:    for (int k = 0; k < 5; k++) cfg.meas[k] = 0; break;
                case I_BRIGHT:  cfg.bright = (uint8_t)((cfg.bright + 1) & 3); backlight_set(BRIGHT_PCT[cfg.bright]); break;
                case I_AUTOOFF: cfg.auto_off = (uint8_t)((cfg.auto_off + 1) & 3); break;
                default: changed = 0;
                }
                if (changed) { mark_dirty(); draw_menu(); continue; }
                if (id == I_MEAS || id == I_SYS || id == I_GENM) {
                    s_menu = id == I_MEAS ? 1 : id == I_SYS ? 2 : 3; s_mi = 0; redraw_all(); draw_menu(); continue;
                }
                if (id == I_BACK) {
                    for (unsigned k = 0; k < sizeof MN_MAIN; k++) if (MN_MAIN[k] == MN_PARENT[s_menu]) s_mi = (uint8_t)k;
                    s_menu = 0; redraw_all(); draw_menu(); continue;
                }
                if (id == I_DMM) { acq_stop(); roll_stop(); capturing = 0; page = PG_DMM; dmm_enter(); continue; }
                if (id == I_ZERO) {
                    page = PG_CONFIRM;
                    lcd_fill(30, 70, 260, 84, C_GRID2);
                    lcd_fill(31, 71, 258, 82, C_BAR);
                    lcd_text(44, 80, "Ajuste de zero", 1, C_TRIG, C_BAR);
                    lcd_text(44, 100, "Ligue a ponta de prova ao jacaré", 1, C_TEXT, C_BAR);
                    lcd_text(44, 112, "(GND), sem nenhum sinal na entrada.", 1, C_TEXT, C_BAR);
                    lcd_text(44, 134, "OK: medir e salvar   Outro: cancelar", 1, C_MUTED, C_BAR);
                    continue;
                }
                if (id == I_FACTORY) { factory_zero(); toast("Zero de fábrica restaurado", C_WAVE); delay_ms(900); }
                if (id == I_BATT) { acq_stop(); roll_stop(); capturing = 0; page = PG_BATT; batt_dialog(); t_diag = 0; continue; }
                if (id == I_DIAG) { acq_stop(); roll_stop(); capturing = 0; page = PG_DIAG; draw_diag_static(); t_diag = 0; continue; }
                if (id == I_ABOUT) { page = PG_ABOUT; draw_about(); continue; }
                page = PG_SCOPE; redraw_all();          // Fechar
                continue;
            }
            if (ev.shortp & (KB(K_MODE) | KB(K_RUN) | KB(K_AUTO))) { page = PG_SCOPE; redraw_all(); continue; }
            continue;
        }

        // ---- scope page
        if (ev.shortp & KB(K_OK)) { page = PG_MENU; s_menu = 0; if (s_mi >= MN_N[0]) s_mi = 0; draw_menu(); continue; }

        int rescale = 0, top = 0, hpos_moved = 0;
        if (s_cmd_c) {                                  // a value chosen on the PC
            char c = s_cmd_c;
            int32_t v = s_cmd_v;
            s_cmd_c = 0;
            switch (c) {
            case 'V': if (v >= 0 && v < (int)NVDIV) { cfg.vdiv = (uint8_t)v; rescale = 1; } break;
            case 'T': if (v >= 0 && v < (int)NTDIV) { cfg.tdiv = (uint8_t)v; rescale = 1; } break;
            case 'P': if (v >= -POS_MAX && v <= POS_MAX) { cfg.pos = (int16_t)v; rescale = 1; s_guide_t = g_ms; } break;
            case 'L':
                if (v == -999999) cfg.trig_auto = 1;
                else { cfg.trig_auto = 0; cfg.trig_mv = v; m_trig = (int32_t)zero() + mv_to_counts(v); s_guide_t = g_ms; }
                break;
            case 'C': cfg.dc = v ? 1 : 0; fe_set_dc(cfg.dc); f_valid = 0; draw_grid(); break;
            case 'E': cfg.rising = v ? 1 : 0; break;
            case 'M': if (v >= 0 && v <= TM_SINGLE) { cfg.trig_mode = (uint8_t)v; s_run = 1; } break;
            case 'X': cfg.probe10 = v ? 1 : 0; break;
            case 'G':
                if ((v >= 0 && v < (int)NGEN) || (v == GEN_CUSTOM && cfg.gen_hz >= GEN_MIN_HZ && cfg.gen_hz <= GEN_MAX_HZ)) {
                    cfg.gen = (uint8_t)v; gen_apply();
                }
                break;
            case 'F': if (v >= (int32_t)GEN_MIN_HZ && v <= (int32_t)GEN_MAX_HZ) { cfg.gen_hz = (uint32_t)v; cfg.gen = GEN_CUSTOM; gen_apply(); } break;
            case 'Y': if (v >= 1 && v <= 99) { cfg.gen_duty = (uint8_t)v; gen_apply(); } break;
            case 'A': if (v >= 0 && v <= 3) { cfg.avg = (uint8_t)v; a_n = 0; } break;
            case 'H': if (v >= -HPOS_MAX && v <= HPOS_MAX) { cfg.hpos = (int16_t)v; a_n = 0; hpos_moved = 1; } break;
            case 'U': if (v >= 0 && v <= CUR_VOLT) { cfg.cursors = (uint8_t)v; if (s_sel >= nsel()) s_sel = P_VDIV; draw_grid(); } break;
            case 'R': s_run = v ? 1 : 0; s_waiting = 0; if (!s_run) roll_stop(); break;
            case 'S': if (v >= 0 && v < nsel()) s_sel = (uint8_t)v; break;
            case 'Q': if ((v >> 4) < 5 && (v & 15) < NMS) cfg.meas[v >> 4] = (uint8_t)(v & 15); break;
            }
            if (c != 'R' && c != 'S') mark_dirty();
            top = 1;
            draw_bottom();
        }
        if (ev.press & KB(K_RIGHT)) { s_sel = (uint8_t)((s_sel + 1) % nsel()); top = 1; }
        if (ev.press & KB(K_LEFT))  { s_sel = (uint8_t)((s_sel + nsel() - 1) % nsel()); top = 1; }
        if (cfg.cursors && (ev.press & (KB(K_LEFT) | KB(K_RIGHT)))) draw_bottom();   // label colours
        int d = (ev.press & KB(K_UP)) ? 1 : (ev.press & KB(K_DOWN)) ? -1 : 0;
        if (d && s_sel >= P_C1) {                       // move a cursor, faster when held
            s_rep_n = g_ms - s_rep_t < 250 ? s_rep_n + 1 : 0;
            s_rep_t = g_ms;
            int step = s_rep_n > 20 ? 8 : s_rep_n > 6 ? 3 : 1;
            int i = s_sel - P_C1;
            if (cfg.cursors == CUR_TIME) {
                int v = s_cur_t[i] + d * step;
                s_cur_t[i] = (int16_t)(v < 0 ? 0 : v > GW - 1 ? GW - 1 : v);
            } else {
                int v = s_cur_v[i] - d * step;
                s_cur_v[i] = (int16_t)(v < 0 ? 0 : v > GH ? GH : v);
            }
            draw_bottom();
            d = 0;
        }
        if (d) {
            switch (s_sel) {
            case P_VDIV:
                if ((d > 0 && cfg.vdiv + 1 < (int)NVDIV) || (d < 0 && cfg.vdiv > 0)) { cfg.vdiv += d; rescale = 1; }
                break;
            case P_TDIV:
                if ((d > 0 && cfg.tdiv + 1 < (int)NTDIV) || (d < 0 && cfg.tdiv > 0)) { cfg.tdiv += d; rescale = 1; }
                break;
            case P_POS: {
                int p = cfg.pos + d * POS_STEP;
                if (p > POS_MAX) p = POS_MAX;
                if (p < -POS_MAX) p = -POS_MAX;
                if (p != cfg.pos) { cfg.pos = (int16_t)p; rescale = 1; s_guide_t = g_ms; }
                break; }
            case P_HPOS: {
                int h = cfg.hpos + d * HPOS_STEP;
                if (h > HPOS_MAX) h = HPOS_MAX;
                if (h < -HPOS_MAX) h = -HPOS_MAX;
                if (h != cfg.hpos) { cfg.hpos = (int16_t)h; a_n = 0; hpos_moved = 1; }
                draw_bottom();
                break; }
            case P_TRIG: {
                if (cfg.trig_auto) { cfg.trig_mv = counts_to_mv(m_trig - zero()); cfg.trig_auto = 0; }
                cfg.trig_mv += d * (int32_t)VDIV_MV[cfg.vdiv] / 5;     // 0.2 division a step
                m_trig = (int32_t)zero() + mv_to_counts(cfg.trig_mv);
                s_guide_t = g_ms;
                top = 1;
                break; }
            }
            mark_dirty();
        }
        if (hpos_moved) {
            if (!s_run && s_start >= 0 && cfg.tdiv < TD_ROLL) {     // stopped: walk through the capture
                int st = s_trig_at - (NS / 2 + cfg.hpos);
                if (st < 0) st = 0;
                if (st > NCAP - NS) st = NCAP - NS;
                s_start = st;
                s_mark_x = s_trig_at - st;
                draw_trace(g_acq + st, 0);
                pc_frame(g_acq + st, NS);
                s_frame_new = 1;
            } else s_mark_x = NS / 2 + cfg.hpos;
            draw_markers();
        }
        if (ev.press & (KB(K_LEFT) | KB(K_RIGHT))) { draw_bottom(); draw_markers(); }     // DISPARO label, marker colour
        if (ev.shortp & KB(K_MODE)) { cfg.dc = !cfg.dc; fe_set_dc(cfg.dc); f_valid = 0; a_n = 0; draw_grid(); top = 1; mark_dirty(); }
        if (ev.shortp & KB(K_RUN))  { s_run = !s_run; s_waiting = 0; top = 1; if (!s_run) roll_stop(); }
        if (ev.shortp & KB(K_AUTO)) {
            acq_stop(); roll_stop(); capturing = 0;
            autoset();
            a_n = 0;
            s_run = 1;
            redraw_all();
            continue;
        }
        if (rescale) {
            acq_stop(); roll_stop(); capturing = 0; r_n = 0; a_n = 0;
            apply_scales();
            draw_grid();
            top = 1;
        }
        if (top) { draw_top(); draw_markers(); }
        guide_update();
        cursor_update();

        // ---- acquisition
        if (!s_run) { battery_tick(); continue; }       // the ADC is free while stopped
        if (cfg.tdiv >= TD_ROLL) {
            if (!s_roll_on) { roll_start(); s_roll_t = g_ms; }
            if (g_ms - s_roll_t < 40) continue;         // about 25 redraws a second
            s_roll_t = g_ms;
            int k = roll_take();
            uint32_t bt = s_bat_t;
            battery_tick();                             // stops the sampling when due:
            if (s_bat_t != bt) roll_start();            // start it again
            if (!k) continue;
            int from = NS - r_n;
            stats(r_buf + from, r_n);
            m_trig = cfg.trig_auto ? (m_min + m_max) / 2 : zero() + mv_to_counts(cfg.trig_mv);
            smooth();
            draw_trace(r_buf, from);
            s_frame_new = 1;
            banner();
            draw_markers();
            pc_frame(r_buf + from, r_n);
            if (g_ms - t_bottom > 250) { t_bottom = g_ms; draw_bottom(); draw_top(); pc_meas(); }
            continue;
        }
        if (!capturing) { s_fs = acq_start(s_fs, NCAP); capturing = 1; }
        if (!acq_done()) continue;
        capturing = 0;
        battery_tick();

        int start = analyse();
        if (start < 0) {                        // Normal / Single: keep the old trace
            if (!s_waiting) { s_waiting = 1; draw_top(); }
            draw_markers();
            continue;
        }
        if (s_waiting) { s_waiting = 0; draw_top(); }
        const uint16_t* tr = g_acq + start;
        if (cfg.avg) {                                  // running average of the triggered traces
            int n = AVG_N[cfg.avg & 3];
            if (a_n < n) a_n++;
            for (int i = 0; i < NS; i++) {
                int32_t v16 = (int32_t)tr[i] * 16;
                a_buf[i] = (uint16_t)(a_n == 1 ? v16 : a_buf[i] + (v16 - (int32_t)a_buf[i]) / a_n);
                a_out[i] = (uint16_t)((a_buf[i] + 8) >> 4);
            }
            tr = a_out;
            stats(tr, NS);                              // measure the cleaned trace
        }
        smooth();
        s_start = start;
        s_mark_x = NS / 2 + cfg.hpos;
        draw_trace(tr, 0);
        s_frame_new = 1;
        pc_frame(tr, NS);
        banner();
        draw_markers();
        if (cfg.trig_mode == TM_SINGLE) { s_run = 0; draw_top(); draw_bottom(); pc_meas(); continue; }
        if (g_ms - t_bottom > 250) {
            t_bottom = g_ms;
            draw_bottom();
            draw_top();
            pc_meas();
        }
    }
}
