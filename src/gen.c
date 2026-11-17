#include "gen.h"
#include "hw.h"

void gen_set(uint32_t hz) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    TIM2->CR1 = 0;
    if (!hz) {
        pin_out(GPIOA, 0, 0);                   // off: output held low
        return;
    }
    // TIM2 runs from APB1 x2 = 72 MHz (APB1 is divided by 2)
    uint32_t tclk = SystemCoreClock;
    uint32_t psc = 0, div = (tclk + hz / 2) / hz;
    while (div > 65536u) { psc++; div = (tclk / (psc + 1u) + hz / 2) / hz; }
    if (div < 2u) div = 2u;
    TIM2->PSC   = psc;
    TIM2->ARR   = div - 1u;
    TIM2->CCR1  = div / 2u;
    TIM2->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE;   // PWM mode 1
    TIM2->CCER  = TIM_CCER_CC1E;
    TIM2->EGR   = TIM_EGR_UG;
    TIM2->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;
    pin_mode(GPIOA, 0, 0xB);                    // alternate function push-pull, 50 MHz
}
