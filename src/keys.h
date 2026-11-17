// The keys, as on the case. All active low with pull-ups (checked on the unit).
#pragma once
#include <stdint.h>

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_MODE, K_AUTO, K_RUN, K_POWER, K_COUNT };
#define KB(k) (1u << (k))

typedef struct {
    uint16_t press;     // went down now (arrows: also auto-repeat)
    uint16_t shortp;    // released before the long-press time (not arrows)
    uint16_t longp;     // held for the long-press time (once per hold)
    uint16_t held;      // down at the moment
} KeyEvents;

void keys_init(void);
void keys_poll(KeyEvents* ev);          // call every loop
int  keys_down(int k);
