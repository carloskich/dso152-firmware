#include "frontend.h"
#include "hw.h"

// ADC counts per volt at the probe tip (x1), per range. Start: the stock
// gain table (0x0800D2E8) x 5. Measured on the user's unit against a
// multimeter (2026-11-16): range 0 = 9960 (stock table says 8000), range 1 =
// 4990 (stock 4000); ranges 2, 4, 5 within 0.4 % of the table. Ranges 3, 6, 9,
// 10 are not used by the V/div steps; 7 and 8 still to be checked.
static const uint16_t CPV[FE_RANGES] = {9960, 4990, 2000, 1000, 500, 200, 100, 40, 20, 10, 5};
static uint8_t s_range = 7;
static uint8_t s_dc = 1;

void fe_init(void) {
    for (int b = 2; b <= 6; b++) pin_out(GPIOA, b, 0);
    fe_set_dc(1);
    fe_set_range(s_range);
}

void fe_set_range(uint8_t r) {
    if (r >= FE_RANGES) r = FE_RANGES - 1;
    s_range = r;
    uint8_t tap = r < 5 ? r : (uint8_t)(r - 5);   // relay off for 0..4, on for 5..10
    pin_set(GPIOA, 2, r >= 5);
    pin_set(GPIOA, 4, tap & 1);
    pin_set(GPIOA, 5, (tap >> 1) & 1);
    pin_set(GPIOA, 6, (tap >> 2) & 1);
}

void fe_set_raw(int relay, uint8_t tap) {
    pin_set(GPIOA, 2, relay != 0);
    pin_set(GPIOA, 4, tap & 1);
    pin_set(GPIOA, 5, (tap >> 1) & 1);
    pin_set(GPIOA, 6, (tap >> 2) & 1);
}

uint8_t fe_range(void) { return s_range; }
void fe_set_dc(int dc) { s_dc = (uint8_t)(dc != 0); pin_set(GPIOA, 3, s_dc); }
int  fe_dc(void) { return s_dc; }
uint32_t fe_counts_per_volt(uint8_t r) { return CPV[r < FE_RANGES ? r : FE_RANGES - 1]; }
