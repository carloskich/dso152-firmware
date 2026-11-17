// USB serial port (CDC ACM) on the CH32F103's STM32-compatible USB device
// block. Windows 10+, Linux, macOS and Android use their own drivers; Chrome
// and Edge reach it through Web Serial.
#pragma once
#include <stdint.h>

void usb_init(void);            // after the 72 MHz clock is up (48 MHz for USB)
// 0: no host (cable out, charger, PC asleep)
// 1: set up by the PC, but no program has the port open
// 2: a program holds the port open (DTR), data is being sent
int  usb_state(void);
int  usb_tx_free(void);         // bytes that fit in the send queue now
int  usb_write(const void* p, int n);   // all or nothing; returns n or 0
int  usb_getc(void);            // a byte from the PC, -1 if none
