#include "keys.h"
#include "hw.h"

typedef struct { GPIO_TypeDef* port; uint8_t bit; } KeyPin;
static const KeyPin PINS[K_COUNT] = {
    {GPIOC, 6}, {GPIOA, 9}, {GPIOC, 3}, {GPIOC, 5},
    {GPIOC, 4}, {GPIOC, 7}, {GPIOC, 8}, {GPIOA, 10}, {GPIOC, 11},
};

#define DEBOUNCE_MS   25
#define LONG_MS      800
#define REPEAT_DELAY 400
#define REPEAT_MS    110

static uint8_t  s_down[K_COUNT], s_long[K_COUNT], s_armed[K_COUNT];
static uint32_t s_t_change[K_COUNT], s_t_down[K_COUNT], s_t_rep[K_COUNT];

int keys_down(int k) { return !pin_get(PINS[k].port, PINS[k].bit); }

void keys_init(void) {
    for (int k = 0; k < K_COUNT; k++) {
        pin_in_pu(PINS[k].port, PINS[k].bit);
    }
    delay_ms(2);
    for (int k = 0; k < K_COUNT; k++) {
        s_down[k] = (uint8_t)keys_down(k);
        s_armed[k] = !s_down[k];        // a key held at start (POWER) is ignored until let go
    }
}

static int is_arrow(int k) { return k <= K_RIGHT; }

void keys_poll(KeyEvents* ev) {
    uint32_t now = g_ms;
    ev->press = ev->shortp = ev->longp = ev->held = 0;
    for (int k = 0; k < K_COUNT; k++) {
        int d = keys_down(k);
        if (d != s_down[k] && (now - s_t_change[k]) >= DEBOUNCE_MS) {
            s_down[k] = (uint8_t)d;
            s_t_change[k] = now;
            if (d) {
                s_t_down[k] = s_t_rep[k] = now;
                s_long[k] = 0;
                if (s_armed[k]) ev->press |= KB(k);
            } else {
                if (s_armed[k] && !s_long[k] && !is_arrow(k)) ev->shortp |= KB(k);
                s_armed[k] = 1;
            }
        }
        if (s_down[k] && s_armed[k]) {
            ev->held |= KB(k);
            if (is_arrow(k)) {
                if (now - s_t_down[k] >= REPEAT_DELAY && now - s_t_rep[k] >= REPEAT_MS) {
                    s_t_rep[k] = now;
                    ev->press |= KB(k);
                }
            } else if (!s_long[k] && now - s_t_down[k] >= LONG_MS) {
                s_long[k] = 1;
                ev->longp |= KB(k);
            }
        }
    }
}
