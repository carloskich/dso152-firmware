#include "settings.h"
#include "hw.h"

#define MAGIC 0x32353144u       // "D152"
#define VERSION 5

static uint32_t checksum_w(const uint32_t* w, unsigned n) {
    uint32_t sum = 0x5A5A0000u;
    for (unsigned i = 0; i < n; i++) sum = (sum << 1 | sum >> 31) ^ w[i];
    return sum;
}
static uint32_t checksum(const Settings* s) {
    return checksum_w((const uint32_t*)s, (sizeof(Settings) - 4) / 4);
}

int settings_load(Settings* s) {
    const Settings* f = (const Settings*)SETTINGS_ADDR;
    const uint32_t* w = (const uint32_t*)SETTINGS_ADDR;
    if (f->magic != MAGIC) return 0;
    if (f->version == VERSION && f->check == checksum(f)) { *s = *f; return 1; }
    // version 4 (v1.0 - v1.9): the same record without the 8 bytes added in
    // v2.0 before the check. Taken over so the user's zero and battery
    // calibration survive the update; the new fields start at 0 (defaults).
    unsigned n4 = (sizeof(Settings) - 12) / 4;
    if (f->version == 4 && w[n4] == checksum_w(w, n4)) {
        uint32_t* d = (uint32_t*)s;
        for (unsigned i = 0; i < sizeof(Settings) / 4; i++) d[i] = i < n4 ? w[i] : 0;
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------ flash ----
static int in_our_page(uint32_t a, uint32_t len) {
    return a >= SETTINGS_ADDR && a + len <= SETTINGS_END;
}

static int flash_wait(void) {
    for (uint32_t t = 0; t < 2000000; t++) {
        if (!(FLASH->SR & FLASH_SR_BSY)) return !(FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR));
        wdg_kick();
    }
    return 0;
}

int settings_save(const Settings* in) {
    Settings s = *in;
    s.magic = MAGIC;
    s.version = VERSION;
    s.check = checksum(&s);
    if (!in_our_page(SETTINGS_ADDR, sizeof s)) return 0;        // never anything else

    __disable_irq();
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;

    int ok = flash_wait();
    if (ok) {                                                   // erase our page only
        FLASH->CR = FLASH_CR_PER;
        FLASH->AR = SETTINGS_ADDR;
        FLASH->CR = FLASH_CR_PER | FLASH_CR_STRT;
        ok = flash_wait();
        FLASH->CR = 0;
    }
    const uint16_t* src = (const uint16_t*)&s;
    volatile uint16_t* dst = (volatile uint16_t*)SETTINGS_ADDR;
    for (unsigned i = 0; ok && i < sizeof s / 2; i++) {
        if (!in_our_page((uint32_t)&dst[i], 2)) { ok = 0; break; }
        FLASH->CR = FLASH_CR_PG;
        dst[i] = src[i];
        ok = flash_wait();
        FLASH->CR = 0;
    }
    FLASH->CR = FLASH_CR_LOCK;
    __enable_irq();

    if (ok) {                                                   // read back
        const uint16_t* f = (const uint16_t*)SETTINGS_ADDR;
        for (unsigned i = 0; i < sizeof s / 2; i++) if (f[i] != src[i]) return 0;
    }
    return ok;
}
