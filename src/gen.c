#include "gen.h"
#include "hw.h"

uint32_t gen_set(uint32_t hz, uint32_t duty) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    TIM2->CR1 = 0;
    if (!hz) {
        pin_out(GPIOA, 0, 0);                   // off: output held low
        return 0;
    }
    if (hz > GEN_MAX_HZ) hz = GEN_MAX_HZ;
    if (duty < 1 || duty > 99) duty = 50;
    // TIM2 runs from APB1 x2 = 72 MHz (APB1 is divided by 2)
    uint32_t tclk = SystemCoreClock;
    uint32_t psc = 0, div = (tclk + hz / 2) / hz;
    while (div > 65536u) { psc++; div = (tclk / (psc + 1u) + hz / 2) / hz; }
    if (div < 2u) div = 2u;
    uint32_t hi = (div * duty + 50u) / 100u;
    if (hi < 1u) hi = 1u;
    if (hi > div - 1u) hi = div - 1u;
    TIM2->PSC   = psc;
    TIM2->ARR   = div - 1u;
    TIM2->CCR1  = hi;
    TIM2->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE;   // PWM mode 1
    TIM2->CCER  = TIM_CCER_CC1E;
    TIM2->EGR   = TIM_EGR_UG;
    TIM2->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;
    pin_mode(GPIOA, 0, 0xB);                    // alternate function push-pull, 50 MHz
    return (uint32_t)((uint64_t)tclk * 1000u / ((psc + 1u) * div));
}
