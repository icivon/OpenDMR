/*
 * OpenDMR, DM-36 only: UART2 (0x14090000), the LytLink Bluetooth module at
 * 115200 8N1. The stock firmware polls the TX FIFO and the receive happens in
 * the Bluetooth task (no UART2 interrupt); the block is the same 16550-like
 * one as UART0 / UART1, so only its base and setup live here.
 *
 * The register helpers and clock_apb_hz come from the shared hal/hw headers;
 * nothing in drivers/uart.c is modified.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "uart2_dm36.h"

#define UART2_BASE 0x14090000

#define UART_RBR(b) REG((b) + 0x00)
#define UART_THR(b) REG((b) + 0x00)
#define UART_DLL(b) REG((b) + 0x00)
#define UART_DLH(b) REG((b) + 0x04)
#define UART_IER(b) REG((b) + 0x04)
#define UART_FCR(b) REG((b) + 0x08)
#define UART_LCR(b) REG((b) + 0x0c)
#define UART_LSR(b) REG((b) + 0x14)
#define UART_USR(b) REG((b) + 0x7c)

void uart2_init(uint32_t baud)
{
    uint32_t divisor = ((clock_apb_hz >> 4) / baud);

    UART_LCR(UART2_BASE) |= (1 << 7);
    UART_DLL(UART2_BASE) = divisor & 0xff;
    UART_DLH(UART2_BASE) = (divisor >> 8) & 0xff;
    UART_LCR(UART2_BASE) &= ~(1 << 7);
    UART_IER(UART2_BASE) = 0x00; // polled; the Bluetooth task calls uart2_read()
    UART_LCR(UART2_BASE) |= 0x03;                            // 8 bits
    UART_FCR(UART2_BASE) = (0x01 << 6) | (0x02 << 4) | 0x07; // FIFOs reset and on, rx trigger 1, tx trigger 2
}

void RAM_CODE uart2_write(const uint8_t *data, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        while ((UART_USR(UART2_BASE) & 0x02) == 0) {
            // transmit FIFO full
        }
        UART_THR(UART2_BASE) = data[i];
    }
}

bool RAM_CODE uart2_read(uint8_t *c)
{
    if ((UART_LSR(UART2_BASE) & 0x01) == 0) {
        return false;
    }
    *c = (uint8_t)UART_RBR(UART2_BASE);
    return true;
}
