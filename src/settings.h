// Our settings page in flash: 0x0800F800 .. 0x0800FBFF (1 KB).
// The FNIRSI calibration page at 0x0800FC00 is never written; the flash
// routines refuse any address outside our page.
#pragma once
#include <stdint.h>
#include "frontend.h"

#define SETTINGS_ADDR 0x0800F800u
#define SETTINGS_END  0x0800FC00u

typedef struct {
    uint32_t magic;             // 'D152'
    uint16_t version;
    uint8_t  vdiv, tdiv;
    uint8_t  dc, rising, trig_mode, probe10;
    uint8_t  trig_auto, user_zero;  // user_zero: zero_* measured by the user
    uint8_t  gen, cursors;          // generator frequency step (0 = off); cursors 0 off, 1 time, 2 volts
    uint16_t bat_ratio, bat_pad;    // battery divider x1000 (0 = not calibrated)
    int16_t  pos;               // vertical position, pixels (+ = up)
    int32_t  trig_mv;           // manual trigger level, mV at the probe (x1)
    int16_t  zero_dc[FE_RANGES];
    int16_t  zero_ac[FE_RANGES];
    uint8_t  meas[5];           // bottom bar measurements (MS_* in main.c), 0 = default
    uint8_t  auto_off;          // 0 off, 1 5 min, 2 10 min, 3 30 min
    uint8_t  bright;            // backlight: 0 100 %, 1 75 %, 2 50 %, 3 25 %
    uint8_t  pad5;
    // v2.1
    uint32_t gen_hz;            // typed generator frequency (cfg.gen == GEN_CUSTOM), Hz
    uint8_t  gen_duty;          // generator duty, 1..99 % (0 = 50 %)
    uint8_t  avg;               // trace averaging: 0 off, 1 x4, 2 x8, 3 x16
    int16_t  hpos;              // trigger x on the screen, pixels from the centre
    uint8_t  dmm_focus;         // multimeter: the big value
    uint8_t  dmm_range;         // multimeter: 0 auto, else fixed range n-1
    uint8_t  pad6[2];
    uint32_t check;
} Settings;

int  settings_load(Settings* s);            // 0 none, 1 current, 4 / 5 older record taken over
int  settings_save(const Settings* s);      // 1 if written and verified
