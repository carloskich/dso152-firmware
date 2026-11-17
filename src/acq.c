// ADC set-up as the stock firmware and the DSO152 community projects use it:
// ADCCLK = PCLK2 / 2 = 36 MHz, 1.5-cycle sample time (14 cycles a
// conversion, up to ~2.5 MSa/s), external trigger TIM1 CC1.
#include "acq.h"
#include "hw.h"

uint16_t g_acq[ACQ_MAX];

void acq_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_ADC1EN | RCC_APB2ENR_TIM1EN;
    RCC->AHBENR  |= RCC_AHBENR_DMA1EN;

    pin_mode(GPIOA, 1, 0x0);            // PA1 analogue input
    RCC->CFGR &= ~RCC_CFGR_ADCPRE;      // ADCCLK = PCLK2 / 2

    ADC1->CR1   = 0;
    ADC1->SQR1  = 0;                    // one conversion
    ADC1->SQR3  = 1;                    // channel 1
    ADC1->SMPR2 = 0;                    // 1.5 cycles
    ADC1->CR2   = ADC_CR2_EXTTRIG | ADC_CR2_DMA;   // EXTSEL 000 = TIM1 CC1
    ADC1->CR2  |= ADC_CR2_ADON;
    delay_us(20);
    ADC1->CR2  |= ADC_CR2_RSTCAL;
    for (uint32_t t = 0; (ADC1->CR2 & ADC_CR2_RSTCAL) && t < 100000; t++) {}
    ADC1->CR2  |= ADC_CR2_CAL;
    for (uint32_t t = 0; (ADC1->CR2 & ADC_CR2_CAL) && t < 100000; t++) {}

    // TIM1 is an advanced timer: MOE must be set for OC1REF to reach the
    // ADC. PA8 (LCD reset) stays a plain output, so nothing comes out there.
    TIM1->CR1   = 0;
    TIM1->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1;  // PWM mode 1
    TIM1->CCER  = TIM_CCER_CC1E;
    TIM1->BDTR  = TIM_BDTR_MOE;
}

void acq_stop(void) {
    TIM1->CR1 &= ~TIM_CR1_CEN;
    DMA1_Channel1->CCR &= ~DMA_CCR_EN;
}

static uint32_t start(uint32_t hz, uint16_t n, uint32_t circ) {
    acq_stop();
    if (n > ACQ_MAX) n = ACQ_MAX;
    if (hz > ACQ_MAX_HZ) hz = ACQ_MAX_HZ;
    if (hz < 1) hz = 1;

    uint32_t tclk = SystemCoreClock;    // APB2 undivided
    uint32_t psc = 0, div = (tclk + hz / 2) / hz;
    while (div > 65536u) { psc++; div = (tclk / (psc + 1u) + hz / 2) / hz; }
    if (div < 2u) div = 2u;

    DMA1->IFCR = DMA_IFCR_CGIF1;
    DMA1_Channel1->CPAR  = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR  = (uint32_t)g_acq;
    DMA1_Channel1->CNDTR = n;
    DMA1_Channel1->CCR   = DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 | DMA_CCR_MINC |
                           DMA_CCR_PL_0 | DMA_CCR_PL_1 | circ;
    DMA1_Channel1->CCR  |= DMA_CCR_EN;
    (void)ADC1->DR;                     // drop a stale result

    TIM1->PSC  = psc;
    TIM1->ARR  = div - 1u;
    TIM1->CCR1 = div / 2u;
    TIM1->CNT  = 0;
    TIM1->EGR  = TIM_EGR_UG;
    TIM1->CR1  = TIM_CR1_CEN;
    return tclk / (psc + 1u) / div;
}

uint16_t acq_read_channel(uint8_t ch) {
    acq_stop();
    uint32_t smpr1 = ADC1->SMPR1, smpr2 = ADC1->SMPR2, sqr3 = ADC1->SQR3;
    if (ch < 10) ADC1->SMPR2 = 7u << (3 * ch);              // 239.5 cycles
    else         ADC1->SMPR1 = 7u << (3 * (ch - 10));
    ADC1->SQR3 = ch;
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_EXTTRIG | (7u << ADC_CR2_EXTSEL_Pos) |
                (ch >= 16 ? ADC_CR2_TSVREFE : 0);           // software start
    delay_us(20);
    // High-impedance sources (the battery divider on PA7) need several
    // conversions before the sampling capacitor forgets the previous
    // channel: seen on the unit as PA7 reading 1353 after a 0 V channel and
    // 2601 after 1.2 V. Throw the first 48 away, average the next 32.
    uint32_t sum = 0;
    for (int i = 0; i < 48 + 32; i++) {
        (void)ADC1->DR;
        ADC1->CR2 |= ADC_CR2_SWSTART;
        for (uint32_t t = 0; !(ADC1->SR & ADC_SR_EOC) && t < 100000; t++) {}
        uint32_t v = ADC1->DR;
        if (i >= 48) sum += v;
    }
    sum /= 2;                                                   // 32 samples -> /16 below
    ADC1->SMPR1 = smpr1; ADC1->SMPR2 = smpr2; ADC1->SQR3 = sqr3;
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_EXTTRIG | ADC_CR2_DMA;   // back to TIM1 CC1
    return (uint16_t)(sum / 16);
}

uint32_t acq_start(uint32_t hz, uint16_t n) { return start(hz, n, 0); }

// Roll mode: the whole buffer filled round and round without stopping.
uint32_t acq_start_circ(uint32_t hz) { return start(hz, ACQ_MAX, DMA_CCR_CIRC); }
uint16_t acq_pos(void) {
    uint16_t left = (uint16_t)DMA1_Channel1->CNDTR;
    return left >= ACQ_MAX ? 0 : (uint16_t)(ACQ_MAX - left);
}

int acq_done(void) {
    if (DMA1->ISR & DMA_ISR_TCIF1) { acq_stop(); return 1; }
    return 0;
}
