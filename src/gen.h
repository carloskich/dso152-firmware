// Square-wave / pulse generator on PA0 (TIM2 channel 1), the output terminal
// on the case. 0 Hz = off (pin held low). Duty 1..99 %.
#pragma once
#include <stdint.h>

#define GEN_MIN_HZ 1u
#define GEN_MAX_HZ 200000u

// Returns the frequency actually produced, in mHz (the timer divides 72 MHz
// by whole numbers), 0 when off.
uint32_t gen_set(uint32_t hz, uint32_t duty);
