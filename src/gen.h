// Square-wave generator on PA0 (TIM2 channel 1, 50 % duty), the output
// terminal on the case. 0 Hz = off (pin held low).
#pragma once
#include <stdint.h>

void gen_set(uint32_t hz);
