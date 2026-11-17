// Sampling: ADC1 channel 1 (PA1, the BNC input after the front end),
// triggered by TIM1 CC1, results moved by DMA1 channel 1. One-shot blocks:
// start a capture of n samples, poll until done, read the buffer.
#pragma once
#include <stdint.h>

#define ACQ_MAX 640
#define ACQ_MAX_HZ 2400000u

extern uint16_t g_acq[ACQ_MAX];

void     acq_init(void);
uint32_t acq_start(uint32_t hz, uint16_t n);   // returns the rate actually set
int      acq_done(void);
uint32_t acq_start_circ(uint32_t hz);          // roll mode: fill g_acq round and round
uint16_t acq_pos(void);                        // index the next sample goes to
void     acq_stop(void);
// One slow, averaged software conversion of any ADC1 channel (0..17), for
// diagnostics. Stops the sampling; the next acq_start() restores it.
uint16_t acq_read_channel(uint8_t ch);
