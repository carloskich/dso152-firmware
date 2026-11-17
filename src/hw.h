// Board support: start-up, clock, watchdog, time, GPIO helpers, power.
#pragma once
#include "stm32f1xx.h"
#include <stdint.h>

#define APP_BASE 0x08003C00u

extern volatile uint32_t g_ms;          // milliseconds since start (SysTick IRQ)

void hw_init(void);                     // everything the board needs, ends at 72 MHz
int  hw_fast_clock(void);               // 1 if running at 72 MHz from the crystal
void wdg_kick(void);
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);
void power_off(void);
void backlight_set(int pct);            // 1..100 %                   // backlight off, release the latch; never returns

void pin_mode(GPIO_TypeDef* p, int bit, uint32_t cfg);
void pin_out(GPIO_TypeDef* p, int bit, int v);
void pin_in_pu(GPIO_TypeDef* p, int bit);
static inline void pin_set(GPIO_TypeDef* p, int bit, int v) {
    p->BSRR = v ? (1u << bit) : (1u << (bit + 16));
}
static inline int pin_get(GPIO_TypeDef* p, int bit) { return (p->IDR >> bit) & 1u; }
