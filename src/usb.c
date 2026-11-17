// USB serial port (CDC ACM), register level, for the STM32F103-compatible
// USB block of the CH32F103 (registers at 0x40005C00, packet memory at
// 0x40006000 seen by the CPU as 16-bit words on 32-bit steps).
//
// The same block is what the FNIRSI bootloader uses for its "DSO BOOT"
// drive, and the DSO152 community firmware (zeromem0/dso152-usb) runs a CDC
// port on it with the clock from the crystal (PLL 72 MHz / 1.5 = 48 MHz).
// From that work: everything is done in the interrupt, the OUT endpoint is
// armed again right there, and data goes out in packets of at most 63 bytes
// (a short packet ends every transfer, so no zero-length packets are needed).
//
// D+ has a pull-up on the board: to make the PC see a fresh device after
// the bootloader, D+ (PA12) is held low for a moment before starting.
#include "usb.h"
#include "hw.h"

#define UREG(o)   (*(volatile uint32_t*)(0x40005C00u + (o)))
#define EPR(n)    UREG(4u * (n))
#define U_CNTR    UREG(0x40)
#define U_ISTR    UREG(0x44)
#define U_DADDR   UREG(0x4C)
#define U_BTABLE  UREG(0x50)
#define PMA       ((volatile uint32_t*)0x40006000u)

// packet memory (USB addresses, bytes): table, then 64-byte buffers
#define EP0_TX 0x40
#define EP0_RX 0x80
#define EP1_TX 0xC0             // data to the PC (bulk IN 0x81)
#define EP2_RX 0x100            // data from the PC (bulk OUT 0x02)
#define EP3_TX 0x140            // notifications (interrupt IN 0x83), never sent
#define RX64   0x8400           // COUNT_RX: 2 blocks of 32 bytes

#define EP_TYPE_BULK 0x0000
#define EP_TYPE_CTRL 0x0200
#define EP_TYPE_INTR 0x0600
#define TX_STALL 0x0010
#define TX_NAK   0x0020
#define TX_VALID 0x0030
#define RX_STALL 0x1000
#define RX_NAK   0x2000
#define RX_VALID 0x3000
#define EP_CTR_RX 0x8000
#define EP_SETUP  0x0800
#define EP_CTR_TX 0x0080
#define EP_DTOG_RX 0x4000
#define EP_DTOG_TX 0x0040
#define EP_KEEP  (EP_CTR_RX | EP_SETUP | 0x0600 | 0x0100 | EP_CTR_TX | 0x000F)   // non-toggle bits
#define EP_STAT_TX 0x0030
#define EP_STAT_RX 0x3000

// ------------------------------------------------------------ descriptors --
static const uint8_t DEV_DESC[18] = {
    18, 1, 0x00, 0x02,          // USB 2.0
    0x02, 0x00, 0x00, 64,       // class CDC, EP0 64 bytes
    0x83, 0x04, 0x40, 0x57,     // VID 0x0483, PID 0x5740 (virtual COM port)
    0x00, 0x02,                 // device release 2.00
    1, 2, 3, 1};
#define CFG_LEN 67
static const uint8_t CFG_DESC[CFG_LEN] = {
    9, 2, CFG_LEN, 0, 2, 1, 0, 0x80, 50,            // bus powered, 100 mA
    // interface 0: communication
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,                      // header, CDC 1.10
    5, 0x24, 0x01, 0x00, 1,                         // call management
    4, 0x24, 0x02, 0x02,                            // ACM: line coding, control lines
    5, 0x24, 0x06, 0, 1,                            // union 0 -> 1
    7, 5, 0x83, 0x03, 8, 0, 0xFF,                   // notifications
    // interface 1: data
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 5, 0x02, 0x02, 64, 0, 0,                     // bulk OUT
    7, 5, 0x81, 0x02, 64, 0, 0};                    // bulk IN
static const uint8_t LANG_DESC[4] = {4, 3, 0x09, 0x04};
static const char* const STR[] = {0, "FNIRSI DSO152", "DSO152 Osciloscopio", 0};
static uint8_t s_strbuf[2 + 2 * 24];

// ----------------------------------------------------------------- state --
static volatile uint8_t  s_config, s_dtr;
static volatile uint32_t s_sof_ms;
static uint8_t  s_addr;                 // to set after the status stage
static uint8_t  s_line[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8};   // 115200 8N1
static uint8_t  s_line_out;             // SET_LINE_CODING data stage expected
static const uint8_t* s_ep0_p;          // IN data stage still to send
static uint16_t s_ep0_n;
static uint8_t  s_ep0_zlp;

#define TXN 2048                        // power of two
#define RXN 256
static uint8_t  s_tx[TXN], s_rx[RXN];
static volatile uint16_t s_tx_h, s_tx_t, s_rx_h, s_rx_t;
static volatile uint8_t  s_tx_busy, s_rx_held;

// ------------------------------------------------------------------- pma --
static void pma_w16(uint16_t a, uint16_t v) { PMA[a / 2] = v; }
static uint16_t pma_r16(uint16_t a) { return (uint16_t)PMA[a / 2]; }
static void pma_write(uint16_t a, const uint8_t* p, int n) {
    for (int i = 0; i < n; i += 2)
        PMA[(a + i) / 2] = (uint16_t)(p[i] | (i + 1 < n ? p[i + 1] << 8 : 0));
}
static void pma_read(uint16_t a, uint8_t* p, int n) {
    for (int i = 0; i < n; i += 2) {
        uint16_t v = (uint16_t)PMA[(a + i) / 2];
        p[i] = (uint8_t)v;
        if (i + 1 < n) p[i + 1] = (uint8_t)(v >> 8);
    }
}
static void set_tx_count(int ep, int n) { pma_w16((uint16_t)(8 * ep + 2), (uint16_t)n); }
static int  rx_count(int ep) { return pma_r16((uint16_t)(8 * ep + 6)) & 0x3FF; }

// --------------------------------------------------------- endpoint regs --
// STAT and DTOG bits flip when written with 1; CTR bits clear when written
// with 0, so they are written as 1 except the one being cleared.
static void ep_stat(int n, uint16_t tx, uint16_t rx) {      // 0xFFFF: leave
    uint16_t r = (uint16_t)EPR(n);
    uint16_t w = (uint16_t)((r & EP_KEEP & ~EP_SETUP) | EP_CTR_RX | EP_CTR_TX);
    if (tx != 0xFFFF) w |= (r ^ tx) & EP_STAT_TX;
    if (rx != 0xFFFF) w |= (r ^ rx) & EP_STAT_RX;
    EPR(n) = w;
}
static void ep_clear(int n, uint16_t ctr) {
    uint16_t r = (uint16_t)EPR(n);
    EPR(n) = (uint16_t)(((r & EP_KEEP & ~EP_SETUP) | EP_CTR_RX | EP_CTR_TX) & ~ctr);
}
static void ep_init(int n, uint16_t type, uint16_t tx, uint16_t rx) {
    uint16_t r = (uint16_t)EPR(n);
    uint16_t w = (uint16_t)(type | n | EP_CTR_RX | EP_CTR_TX);
    w |= (r ^ tx) & EP_STAT_TX;
    w |= (r ^ rx) & EP_STAT_RX;
    w |= r & (EP_DTOG_TX | EP_DTOG_RX);                     // data toggles back to 0
    EPR(n) = w;
}

// ---------------------------------------------------------------- data in --
static void tx_kick(void) {             // in the interrupt, or with it held off
    if (s_tx_busy || s_config == 0) return;
    uint8_t pk[63];
    int n = 0;
    uint16_t t = s_tx_t;
    while (n < 63 && t != s_tx_h) { pk[n++] = s_tx[t]; t = (uint16_t)((t + 1) & (TXN - 1)); }
    if (!n) return;
    s_tx_t = t;
    pma_write(EP1_TX, pk, n);
    set_tx_count(1, n);
    s_tx_busy = 1;
    ep_stat(1, TX_VALID, 0xFFFF);
}
static uint16_t rx_free(void) { return (uint16_t)((s_rx_t - s_rx_h - 1) & (RXN - 1)); }
static void rx_take(void) {
    uint8_t pk[64];
    int n = rx_count(2);
    pma_read(EP2_RX, pk, n);
    for (int i = 0; i < n; i++) { s_rx[s_rx_h] = pk[i]; s_rx_h = (uint16_t)((s_rx_h + 1) & (RXN - 1)); }
    if (rx_free() >= 64) ep_stat(2, 0xFFFF, RX_VALID);
    else s_rx_held = 1;
}

// --------------------------------------------------------------- control --
static void ep0_send_next(void) {
    int n = s_ep0_n > 64 ? 64 : s_ep0_n;
    if (s_ep0_p) pma_write(EP0_TX, s_ep0_p, n);
    set_tx_count(0, n);
    s_ep0_p += n;
    s_ep0_n = (uint16_t)(s_ep0_n - n);
    if (n < 64) s_ep0_zlp = 0;              // that short packet ends it
    ep_stat(0, TX_VALID, RX_VALID);
}
static void ep0_in(const uint8_t* p, int n, int want) {
    if (n > want) n = want;
    s_ep0_p = p;
    s_ep0_n = (uint16_t)n;
    s_ep0_zlp = (n < want) && (n % 64 == 0);
    ep0_send_next();
}
static void ep0_stall(void) { ep_stat(0, TX_STALL, RX_VALID); }
static void ep0_ack(void) { ep0_in(0, 0, 0); }               // zero-length status

static void configure(void) {
    pma_w16(8 * 1 + 0, EP1_TX); set_tx_count(1, 0);
    pma_w16(8 * 2 + 4, EP2_RX); pma_w16(8 * 2 + 6, RX64);
    pma_w16(8 * 3 + 0, EP3_TX); set_tx_count(3, 0);
    ep_init(1, EP_TYPE_BULK, TX_NAK, 0);
    ep_init(2, EP_TYPE_BULK, 0, RX_VALID);
    ep_init(3, EP_TYPE_INTR, TX_NAK, 0);
    s_tx_busy = 0;
    s_rx_held = 0;
}

static void setup(void) {
    uint8_t s[8];
    pma_read(EP0_RX, s, 8);
    uint8_t type = s[0], req = s[1];
    uint16_t val = (uint16_t)(s[2] | s[3] << 8), len = (uint16_t)(s[6] | s[7] << 8);
    s_line_out = 0;

    if ((type & 0x60) == 0x20) {                        // CDC class requests
        switch (req) {
        case 0x20: s_line_out = 1; ep_stat(0, 0xFFFF, RX_VALID); return;   // SET_LINE_CODING
        case 0x21: ep0_in(s_line, 7, len); return;                          // GET_LINE_CODING
        case 0x22: s_dtr = val & 1; ep0_ack(); return;                      // SET_CONTROL_LINE_STATE
        case 0x23: ep0_ack(); return;                                       // SEND_BREAK
        }
        ep0_stall();
        return;
    }
    switch (req) {
    case 0x06: {                                        // GET_DESCRIPTOR
        uint8_t dt = (uint8_t)(val >> 8), di = (uint8_t)val;
        if (dt == 1) { ep0_in(DEV_DESC, 18, len); return; }
        if (dt == 2) { ep0_in(CFG_DESC, CFG_LEN, len); return; }
        if (dt == 3) {
            if (di == 0) { ep0_in(LANG_DESC, 4, len); return; }
            if (di <= 3) {
                int n = 0;
                if (di == 3) {                          // serial number from the chip ID
                    static const char H[] = "0123456789ABCDEF";
                    const volatile uint32_t* id = (const volatile uint32_t*)0x1FFFF7E8u;
                    uint32_t v = id[0] ^ id[1] ^ id[2];
                    for (int i = 0; i < 8; i++) { s_strbuf[2 + 2 * n] = (uint8_t)H[(v >> (28 - 4 * i)) & 15]; s_strbuf[3 + 2 * n] = 0; n++; }
                } else {
                    for (const char* c = STR[di]; *c && n < 24; c++, n++) { s_strbuf[2 + 2 * n] = (uint8_t)*c; s_strbuf[3 + 2 * n] = 0; }
                }
                s_strbuf[0] = (uint8_t)(2 + 2 * n);
                s_strbuf[1] = 3;
                ep0_in(s_strbuf, s_strbuf[0], len);
                return;
            }
        }
        ep0_stall();                                    // qualifier etc.: full speed only
        return; }
    case 0x05: s_addr = (uint8_t)(val & 0x7F); ep0_ack(); return;           // SET_ADDRESS
    case 0x09:                                                              // SET_CONFIGURATION
        s_config = (uint8_t)val;
        if (s_config) configure();
        ep0_ack();
        return;
    case 0x08: { static uint8_t c; c = s_config; ep0_in(&c, 1, len); return; }   // GET_CONFIGURATION
    case 0x00: { static const uint8_t z[2] = {0, 0}; ep0_in(z, 2, len); return; } // GET_STATUS
    case 0x0A: { static const uint8_t z = 0; ep0_in(&z, 1, len); return; }      // GET_INTERFACE
    case 0x01: case 0x03: case 0x0B: ep0_ack(); return;  // CLEAR/SET_FEATURE, SET_INTERFACE
    }
    ep0_stall();
}

static void bus_reset(void) {
    U_BTABLE = 0;
    pma_w16(0, EP0_TX); set_tx_count(0, 0);
    pma_w16(4, EP0_RX); pma_w16(6, RX64);
    ep_init(0, EP_TYPE_CTRL, TX_NAK, RX_VALID);
    U_DADDR = 0x80;                                     // enabled, address 0
    s_config = 0;
    s_dtr = 0;
    s_addr = 0;
    s_tx_busy = 0;
    s_tx_t = s_tx_h;                                    // drop what was queued
}

void USB_LP_CAN1_RX0_IRQHandler(void) {
    uint32_t is = U_ISTR;
    if (is & USB_ISTR_RESET) { U_ISTR = (uint16_t)~USB_ISTR_RESET; bus_reset(); return; }
    if (is & USB_ISTR_SOF)   { U_ISTR = (uint16_t)~USB_ISTR_SOF; s_sof_ms = g_ms; }
    if (is & (USB_ISTR_ERR | USB_ISTR_PMAOVR | USB_ISTR_ESOF | USB_ISTR_SUSP | USB_ISTR_WKUP))
        U_ISTR = (uint16_t)~(USB_ISTR_ERR | USB_ISTR_PMAOVR | USB_ISTR_ESOF | USB_ISTR_SUSP | USB_ISTR_WKUP);

    while ((is = U_ISTR) & USB_ISTR_CTR) {
        int ep = (int)(is & USB_ISTR_EP_ID);
        uint16_t r = (uint16_t)EPR(ep);
        if (ep == 0) {
            if (r & EP_CTR_TX) {
                ep_clear(0, EP_CTR_TX);
                if (s_addr) { U_DADDR = 0x80u | s_addr; s_addr = 0; }
                if (s_ep0_n || s_ep0_zlp) { if (!s_ep0_n) s_ep0_zlp = 0; ep0_send_next(); }
            }
            if (r & EP_CTR_RX) {
                ep_clear(0, EP_CTR_RX);
                if (r & EP_SETUP) setup();
                else if (s_line_out) {                  // SET_LINE_CODING data
                    uint8_t b[8]; int n = rx_count(0);
                    if (n > 8) n = 8;
                    pma_read(EP0_RX, b, n);
                    for (int i = 0; i < 7 && i < n; i++) s_line[i] = b[i];
                    s_line_out = 0;
                    ep0_ack();
                } else ep_stat(0, 0xFFFF, RX_VALID);    // status stage of an IN transfer
            }
        } else if (ep == 1) {
            ep_clear(1, EP_CTR_TX);
            s_tx_busy = 0;
            tx_kick();
        } else if (ep == 2) {
            ep_clear(2, EP_CTR_RX);
            rx_take();
        } else {
            ep_clear(ep, EP_CTR_RX | EP_CTR_TX);
        }
    }
}

// ------------------------------------------------------------------ api --
void usb_init(void) {
    if (!hw_fast_clock()) return;                       // no 48 MHz without the crystal
    RCC->APB1RSTR |= RCC_APB1RSTR_USBRST;               // whatever the bootloader left
    RCC->APB1RSTR &= ~RCC_APB1RSTR_USBRST;
    RCC->CFGR &= ~RCC_CFGR_USBPRE;                      // 72 MHz / 1.5 = 48 MHz
    // D+ low for a moment: the PC sees the device unplugged and plugged in
    pin_out(GPIOA, 12, 0);
    delay_ms(20);
    pin_mode(GPIOA, 12, 0x4);                           // floating, for the USB block
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;
    U_CNTR = USB_CNTR_FRES;                             // powered, held in reset
    delay_us(10);
    U_CNTR = 0;
    U_ISTR = 0;
    U_CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SOFM;
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 2);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
}

int usb_state(void) {
    if (!s_config || g_ms - s_sof_ms > 100) return 0;   // no frames: cable out or PC asleep
    return s_dtr ? 2 : 1;
}

int usb_tx_free(void) { return (int)((s_tx_t - s_tx_h - 1) & (TXN - 1)); }

int usb_write(const void* p, int n) {
    if (usb_state() != 2 || n > usb_tx_free()) return 0;
    const uint8_t* b = (const uint8_t*)p;
    uint16_t h = s_tx_h;
    for (int i = 0; i < n; i++) { s_tx[h] = b[i]; h = (uint16_t)((h + 1) & (TXN - 1)); }
    s_tx_h = h;
    NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    tx_kick();
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    return n;
}

int usb_getc(void) {
    if (s_rx_h == s_rx_t) return -1;
    uint8_t c = s_rx[s_rx_t];
    s_rx_t = (uint16_t)((s_rx_t + 1) & (RXN - 1));
    if (s_rx_held && rx_free() >= 64) {
        NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
        s_rx_held = 0;
        ep_stat(2, 0xFFFF, RX_VALID);
        NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    }
    return c;
}
