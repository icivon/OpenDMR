/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * DM36 adaptation by BG2LNY
 *
 * OpenDMR, DM-36 only: UART2, the 16550-like block driving the LytLink
 * Bluetooth module (stock firmware base 0x14090000). Kept out of the shared
 * drivers/uart.c so a DM-32 build has no trace of it.
 */

#ifndef DM36_UART2_H
#define DM36_UART2_H

#include <stdbool.h>
#include <stdint.h>

void uart2_init(uint32_t baud);
void uart2_write(const uint8_t *data, uint32_t length);
bool uart2_read(uint8_t *c);

#endif
