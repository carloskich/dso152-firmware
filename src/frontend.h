// Analogue front end ahead of the ADC: attenuator relay (PA2), AC/DC
// coupling (PA3, high = DC) and the 74HC4051 tap select (PA4..PA6).
// Eleven hardware ranges, 0 = most sensitive .. 10 = least.
#pragma once
#include <stdint.h>

#define FE_RANGES 11

void     fe_init(void);
void     fe_set_range(uint8_t r);
uint8_t  fe_range(void);
void     fe_set_dc(int dc);
int      fe_dc(void);
// ADC counts per volt at the probe tip (x1) for a range. Range 7 = 40 c/V was
// measured on a real unit by the community; the others follow the stock
// firmware's gain table and are not yet checked on this unit.
uint32_t fe_counts_per_volt(uint8_t r);
// Diagnostics only: drive the relay and the mux tap (0..7) directly. The
// stock firmware uses taps 0..5; 6 and 7 are probed for the battery.
// fe_set_range() puts things back.
void     fe_set_raw(int relay, uint8_t tap);
