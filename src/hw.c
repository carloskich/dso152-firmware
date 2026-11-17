// Board support. Verified on the user's unit with the v0.6 hardware test:
// power latch PC10, backlight PC9 (low = on), 8 MHz crystal, PLL x9 = 72 MHz
// with two flash wait states, SysTick interrupt after clearing what the
// FNIRSI bootloader leaves behind.
#include "hw.h"

volatile uint32_t g_ms;
static uint8_t g_fast;

void SysTick_Handler(void) { g_ms++; }

void wdg_kick(void) { IWDG->KR = 0xAAAA; }

static void wdg_start(void) {
    // ~4 s: LSI 40 kHz / 64, reload 2500. A hang -> reset -> PC10 floats
    // -> the unit switches itself off instead of staying on, stuck.
    IWDG->KR = 0x5555;
    IWDG->PR = 4;
    IWDG->RLR = 2500;
    IWDG->KR = 0xAAAA;
    IWDG->KR = 0xCCCC;
}

void delay_ms(uint32_t ms) {
    uint32_t t0 = g_ms;
    while ((g_ms - t0) < ms) wdg_kick();
}

void delay_us(uint32_t us) {            // busy wait on SysTick's counter
    uint32_t per_us = SystemCoreClock / 1000000u;
    uint32_t load = SysTick->LOAD + 1u;
    uint32_t last = SysTick->VAL, left = us * per_us;
    while (left) {
        uint32_t now = SysTick->VAL;
        uint32_t d = (last >= now) ? (last - now) : (last + load - now);
        if (d >= left) break;
        left -= d;
        last = now;
    }
}

// ------------------------------------------------------------------ gpio --
void pin_mode(GPIO_TypeDef* p, int bit, uint32_t cfg) {
    volatile uint32_t* cr = bit < 8 ? &p->CRL : &p->CRH;
    uint32_t sh = (uint32_t)(bit & 7) * 4u;
    *cr = (*cr & ~(0xFu << sh)) | (cfg << sh);
}
void pin_out(GPIO_TypeDef* p, int bit, int v) { pin_set(p, bit, v); pin_mode(p, bit, 0x3); }
void pin_in_pu(GPIO_TypeDef* p, int bit) { p->BSRR = 1u << bit; pin_mode(p, bit, 0x8); }

// ----------------------------------------------------------------- clock --
static int wait_bits(volatile uint32_t* reg, uint32_t mask, uint32_t want) {
    for (uint32_t t = 0; t < 2000000; t++) {
        if ((*reg & mask) == want) return 1;
        wdg_kick();
    }
    return 0;
}

static void clock_init(void) {
    // Back to the internal 8 MHz oscillator, whatever the bootloader left.
    RCC->CR |= RCC_CR_HSION;
    wait_bits(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY);
    RCC->CFGR &= ~RCC_CFGR_SW;
    wait_bits(&RCC->CFGR, RCC_CFGR_SWS, 0);
    RCC->CR &= ~RCC_CR_PLLON;
    wait_bits(&RCC->CR, RCC_CR_PLLRDY, 0);
    RCC->CFGR = 0;
    RCC->CIR = 0x009F0000;
    // Two wait states. The CMSIS name FLASH_ACR_LATENCY_2 is bit 2 (value 4,
    // invalid): using it is what made v0.3-v0.5 hang above 32 MHz.
    FLASH->ACR = FLASH_ACR_PRFTBE | (2u << FLASH_ACR_LATENCY_Pos);
    SystemCoreClock = 8000000;

    RCC->CR |= RCC_CR_HSEON;
    if (!wait_bits(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY)) { RCC->CR &= ~RCC_CR_HSEON; return; }
    // PLL = 8 MHz x9 = 72 MHz; AHB 72, APB1 36, APB2 72; ADC = APB2 / 2 = 36
    // MHz (what the CH32F103 and the stock 2.5 MSa/s rely on); USB 48 MHz.
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;
    RCC->CR |= RCC_CR_PLLON;
    if (!wait_bits(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) return;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    if (!wait_bits(&RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_PLL)) { RCC->CFGR &= ~RCC_CFGR_SW; return; }
    SystemCoreClock = 72000000;
    g_fast = 1;
}

int hw_fast_clock(void) { return g_fast; }

void hw_init(void) {
    __disable_irq();
    __set_BASEPRI(0);
    __set_FAULTMASK(0);
    SCB->VTOR = APP_BASE;
    for (int i = 0; i < 8; i++) { NVIC->ICER[i] = 0xFFFFFFFFu; NVIC->ICPR[i] = 0xFFFFFFFFu; }
    SysTick->CTRL = 0;
    wdg_start();

    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN;
    // JTAG off, SWD kept: frees PA15 (LCD CS) and PB3/PB4 (LCD data).
    AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG) | AFIO_MAPR_SWJ_CFG_JTAGDISABLE;
    pin_out(GPIOC, 10, 1);              // keep the power on
    pin_out(GPIOC, 9, 0);               // backlight on

    clock_init();
    SysTick->LOAD = SystemCoreClock / 1000u - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
    __enable_irq();
}

// Backlight brightness: PWM on PC9 (TIM3 channel 4, full remap), active
// low, 20 kHz. 100 % keeps it on all the time.
void backlight_set(int pct) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    // SWJ_CFG reads back undefined: always write JTAG-off again with it
    AFIO->MAPR = (AFIO->MAPR & ~(AFIO_MAPR_SWJ_CFG | AFIO_MAPR_TIM3_REMAP)) |
                 AFIO_MAPR_SWJ_CFG_JTAGDISABLE | AFIO_MAPR_TIM3_REMAP_FULLREMAP;
    TIM3->CR1 = 0;
    TIM3->PSC = 0;
    TIM3->ARR = 3599;                   // 72 MHz / 3600 = 20 kHz
    TIM3->CCMR2 = TIM_CCMR2_OC4M_2 | TIM_CCMR2_OC4M_1 | TIM_CCMR2_OC4PE;   // PWM 1
    TIM3->CCER = TIM_CCER_CC4E | TIM_CCER_CC4P;                          // active low
    TIM3->CCR4 = pct >= 100 ? 3600u : (uint32_t)pct * 36u;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->CR1 = TIM_CR1_CEN;
    pin_mode(GPIOC, 9, 0xB);            // alternate function push-pull
}

void power_off(void) {
    pin_out(GPIOC, 9, 1);               // backlight off (plain output again)
    pin_set(GPIOC, 10, 0);              // let the power go
    for (;;) {}                         // on USB power the watchdog resets us
}

// ---------------------------------------------------------------- faults --
// Any fault: blink the backlight fast for ~3 s, then let the power go, so
// the unit can never stay stuck on with a dead program.
static void fault_blink_off(void) {
    __disable_irq();
    GPIOC->CRH = (GPIOC->CRH & ~(0xFu << 4)) | (0x3u << 4);     // PC9 plain output
    for (int i = 0; i < 30; i++) {
        GPIOC->BSRR = (i & 1) ? (1u << (9 + 16)) : (1u << 9);
        for (volatile uint32_t d = 0; d < 400000; d++) IWDG->KR = 0xAAAA;
    }
    GPIOC->BSRR = (1u << 9) | (1u << (10 + 16));
    for (;;) {}
}
void HardFault_Handler(void)  { fault_blink_off(); }
void MemManage_Handler(void)  { fault_blink_off(); }
void BusFault_Handler(void)   { fault_blink_off(); }
void UsageFault_Handler(void) { fault_blink_off(); }
void NMI_Handler(void)        { fault_blink_off(); }

// --------------------------------------------------------------- startup --
// Our own Reset_Handler (overrides the weak one in the CMSIS start-up): the
// very first thing holds the power on and lights the backlight, so a
// running image is visible at once. Then the usual C runtime set-up.
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void) {
    __asm volatile ("msr msp, %0" :: "r" (&_estack));
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->BSRR = (1u << 10) | (1u << (9 + 16));            // PC10 high, PC9 low
    GPIOC->CRH = (GPIOC->CRH & ~((0xFu << 8) | (0xFu << 4))) | (0x3u << 8) | (0x3u << 4);

    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0;
    main();
    for (;;) {}
}
