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
 * OpenDMR, hardware abstraction: GPIO and the pin setup at start.
 * After OpenDM32 (user/main.c, user/DRV/C7000_DRV.c, ucos3/bsp).
 */

#include "hw.h"
#include "hal.h"

void RAM_CODE gpio_output(uint32_t pin, bool level)
{
    GPIO_INTEN(pin) &= ~GPIO_BIT(pin);
    GPIO_DDR(pin) |= GPIO_BIT(pin);
    if (level) {
        GPIO_DR(pin) |= GPIO_BIT(pin);
    } else {
        GPIO_DR(pin) &= ~GPIO_BIT(pin);
    }
}

void RAM_CODE gpio_input(uint32_t pin)
{
    GPIO_INTEN(pin) &= ~GPIO_BIT(pin);
    GPIO_DDR(pin) &= ~GPIO_BIT(pin);
}

void RAM_CODE gpio_set(uint32_t pin)
{
    GPIO_DR(pin) |= GPIO_BIT(pin);
}

void RAM_CODE gpio_reset(uint32_t pin)
{
    GPIO_DR(pin) &= ~GPIO_BIT(pin);
}

// Pin mux and directions as GPIO_Init(); the power switch is held on, the RF switches off
void pins_init(void)
{
    IO_DIPLEX0 = BOARD_IO_DIPLEX0;
    IO_DIPLEX1 = BOARD_IO_DIPLEX1;
    IO_DIPLEX2 = BOARD_IO_DIPLEX2;

    GPIOA_DDR = BOARD_GPIOA_DDR;
    GPIOB_DDR = BOARD_GPIOB_DDR;
    GPIOC_DDR = BOARD_GPIOC_DDR;

    GPIOA_DR |= BOARD_GPIOA_DR_SET;
    GPIOC_DR |= BOARD_GPIOC_DR_SET;
    GPIOB_DR = BOARD_GPIOB_DR_SET;

    IOMGR_REN_REG0 = BOARD_IOMGR_REN0;
    IOMGR_REN_REG1 = BOARD_IOMGR_REN1;
    IOMGR_REN_REG2 = BOARD_IOMGR_REN2;
    // As the end of GPIO_Init(): pad 24 without its pull resistor, pads of REG2 bits 26 and 27 with theirs
    IO_DIPLEX1 |= BOARD_IO_DIPLEX1_EXTRA;
    IOMGR_REN_REG0 &= ~BITSET(BOARD_REN0_CLEAR_BIT);
    IOMGR_REN_REG2 |= BOARD_REN2_EXTRA;

    gpio_output(PIN_MIC_MUT, PIN_MIC_MUT_TO_FD6818);
    for (uint32_t pin = PIN_KEYPAD_ROW0; pin <= PIN_KEYPAD_ROW4; pin++) {
        gpio_output(pin, true);
    }
    gpio_input(PIN_PWR_SWCHECK);
    gpio_output(PIN_PWR_SW, true);
#if BOARD_HAS_GPS
    gpio_output(BOARD_PIN_GPS_POWER, true); // GPS off (low = on): gps_set_enabled()
#endif
    gpio_output(PIN_SPI0_CS, true);
    gpio_output(BOARD_PIN_LNA_VHF, false);
    gpio_output(BOARD_PIN_LNA_UHF, false);
    gpio_output(BOARD_PIN_PA_VHF, false);
    gpio_output(BOARD_PIN_PA_UHF, false);
}
