/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, driver: the transceiver chip's soft SPI (trx_spi.h), after OpenDM32 hardware/AT1846S.c. It runs from RAM
 * with the bit timing of the stock firmware (its delay 0x2df70: 5 counts a half bit to write, 10 to read). Each
 * register access is a critical section, as the main firmware.
 */

#include "hw.h"
#include "hal.h"
#include "trx_spi.h"

#define PIN_SCL 0x07
#define PIN_SDA 0x08
#define PIN_SCN 0x18
#define PIN_GP0 0x3B
#define PIN_GP1 0x3A

void trx_spi_init(void)
{
    gpio_input(PIN_GP0);
    gpio_input(PIN_GP1);
    gpio_output(PIN_SDA, false);
    gpio_output(PIN_SCL, false);
    gpio_output(PIN_SCN, true);
}

static void __attribute__((noinline)) RAM_CODE spi_delay(uint32_t counts)
{
    for (volatile uint32_t i = counts; i != 0; i--) {
    }
}

static void RAM_CODE spi_send_byte(uint8_t byte)
{
    gpio_output(PIN_SDA, true);
    gpio_reset(PIN_SCL);
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
        if (byte & mask) {
            gpio_set(PIN_SDA);
        } else {
            gpio_reset(PIN_SDA);
        }
        spi_delay(5);
        gpio_set(PIN_SCL);
        spi_delay(5);
        gpio_reset(PIN_SCL);
    }
    gpio_set(PIN_SDA);
}

static uint16_t RAM_CODE spi_read_word(void)
{
    uint16_t word = 0;

    gpio_input(PIN_SDA);
    gpio_reset(PIN_SCL);
    for (int i = 0; i < 16; i++) {
        gpio_set(PIN_SCL);
        spi_delay(10);
        word = (uint16_t)((word << 1) | (((GPIO_EXT(PIN_SDA) & GPIO_BIT(PIN_SDA)) != 0) ? 1 : 0));
        gpio_reset(PIN_SCL);
        spi_delay(10);
    }
    return word;
}

// From a task or an interrupt (the DMR engine switches the transmitter per burst): interrupts off, not
// taskENTER_CRITICAL()
void RAM_CODE trx_spi_write(uint8_t reg, uint16_t value)
{
    uint32_t psr = irq_save();
    gpio_reset(PIN_SCN);
    spi_send_byte(reg);
    spi_send_byte((uint8_t)(value >> 8));
    spi_send_byte((uint8_t)value);
    gpio_set(PIN_SCN);
    irq_restore(psr);
}

uint16_t RAM_CODE trx_spi_read(uint8_t reg)
{
    uint16_t value;
    uint32_t psr = irq_save();
    gpio_reset(PIN_SCN);
    spi_send_byte(reg | 0x80);
    spi_delay(10);
    value = spi_read_word();
    gpio_set(PIN_SCN);
    irq_restore(psr);
    return value;
}
