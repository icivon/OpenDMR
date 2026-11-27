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
 * OpenDMR, service: the radio, the DMR transmitter (radio_internal.h): the RF task the DMR engine asks for each burst,
 * with the stock firmware's waits and PA ramp; the physical layer transmit (P25, NXDN) on it too.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "leds.h"
#include "trx.h"
#include "modem.h"
#include "systime.h"
#include "p25_phy.h"
#include "factory_cal.h"
#include "dmr.h"
#include "hr_c7000.h"
#include "radio_internal.h"

/* DMR transmit: the engine switches the transmitter from its interrupts (trxActivateDMRTx(), trxFastDMRTx(),
   trxActivateRx() of the main firmware: radioSetTx(), radioFastTx(), radioSetRx() of the DM-32 releases). As the stock
   firmware (DM32.01.L01.048): the HR_C7000's RF transmit start (vector 64) and RF receive start (65) interrupts only
   ask its RF task (0x300dc44), which switches each burst with waits and a PA ramp (RAM 0x2b440 on, 0x2b4a0 off,
   0x2b364 ramp). Switched at once, without the ramp and back on the Rx frequency in between, the bursts spread over
   the band: a radio next to ours didn't decode the repeater's downlink of our call (a DM1801 next to another did). */

#define RF_TASK_PRIORITY (configMAX_PRIORITIES - 1) // the burst timing first
#define RF_SETUP         0x01 // the first burst of a transmission: the transmitter set up first
#define RF_ON            0x02
#define RF_OFF           0x04
#define RF_RX            0x08 // the end of the transmission
#define RF_GATE          0x10 // the physical layer transmit's bursts (DMR in software): switched by its timing

static StaticTask_t rf_task_tcb;
static StackType_t rf_task_stack[256];
static TaskHandle_t rf_task;
static bool rf_on_air = false; // the PA on (rf task)
static uint16_t dmr_drive;       // the PA drive of the DMR transmission (pa_drive() of its frequency)

// The stock PA ramp (0x3074714): the drive is the target x ramp[i] / 248, up through all 40, down from ramp[20]
static const uint8_t pa_ramp[40] = {1,   2,   2,   4,   8,   10,  12,  14,  16,  18,  20,  24,  28,  32,
                                    36,  40,  46,  52,  58,  64,  70,  76,  82,  88,  94,  100, 106, 112,
                                    118, 124, 133, 143, 158, 173, 188, 203, 218, 228, 238, 248};
#define PA_RAMP_FULL 248
#define PA_RAMP_DOWN 20

// The stock busy wait (RAM 0x2df80): 164 nops a unit, about 1 us
static void RAM_CODE rf_wait(uint32_t units)
{
    for (uint32_t i = 0; i < units; i++) {
        __asm__ volatile(".rept 164\n"
                         "nop\n"
                         ".endr\n");
    }
}

// A ramp step: the stock firmware's soft float maths between two writes, about 4 units (estimated)
static void rf_ramp_step(uint8_t level)
{
    modem_pa_drive((uint16_t)(((uint32_t)dmr_drive * level) / PA_RAMP_FULL));
    rf_wait(4);
}

// 0x2b440: the LNA off, a wait by band (stock: 1.6 / 1.0 / 1.2 ms picked from a channel field, not mapped yet; here VHF /
// 220 MHz / UHF), the Tx frequency, the PA switch, the ramp up. The wait counts from the Tx interrupt.
// Direct mode (Rx == Tx), measured 2026-11-20 with the software receiver against known content at -116 dBm, UHF: the
// burst's data starts some 1.1 ms after the Tx interrupt, the PA at full 0.77 ms after it with no wait (the FD6818 to
// the Tx frequency ~0.55 ms, the ramp ~0.2 ms). With 1200 the first 8 bits of each burst were 46 % wrong (the first
// AMBE frame bad 46 %), 400 4 - 7 %, none 0.4 - 2.7 % (as the rest of the burst). So no wait in direct mode, all bands
// (an earlier PA only puts the carrier up sooner). Repeater mode keeps the stock waits (not measured: no repeater).
// Not const: tuned over the cable (dm32dbg.py mwb rf_on_waits ...)
uint16_t rf_on_waits[3] = {1600, 1000, 1200};
uint16_t rf_on_waits_direct[3] = {0, 0, 0};

static void rf_on_wait(uint32_t wait);
static void rf_pa_up(void);

static void rf_on(void)
{
    int band = radio_band_of(rs.channel.tx_freq);
    rf_on_wait((rs.channel.rx_freq == rs.channel.tx_freq) ? rf_on_waits_direct[band] : rf_on_waits[band]);
}

static void rf_on_wait(uint32_t wait)
{
    lna(false, 0);
    rf_wait(wait);
    trx_dmr_burst_tx(rs.channel.tx_freq);
    rf_pa_up();
}

// the PA switch, the ramp up
static void rf_pa_up(void)
{
    gpio_output((rs.channel.tx_freq >= UHF_FROM) ? PIN_PA_UHF : PIN_PA_VHF, true);
    for (int i = 0; i < (int)sizeof(pa_ramp); i++) {
        rf_ramp_step(pa_ramp[i]);
    }
    modem_pa_drive(dmr_drive);
    rf_on_air = true;
    hrc7000TraceRf(5);
}

static void rf_off_now(void);

// 0x2b4a0: 1.6 ms more (the interrupt comes before the end of the burst), the ramp down, the PA switch off, the Rx
// frequency, the LNA on
static void rf_off(void)
{
    if (!rf_on_air) {
        return;
    }
    rf_wait(1600);
    rf_off_now();
}

static void rf_off_now(void)
{
    if (!rf_on_air) {
        return;
    }
    for (int i = PA_RAMP_DOWN; i > 0; i--) {
        rf_ramp_step(pa_ramp[i]);
    }
    modem_pa_drive(0);
    rf_wait(300);
    gpio_output(PIN_PA_UHF, false);
    gpio_output(PIN_PA_VHF, false);
    rf_on_air = false;
    hrc7000TraceRf(6);
    rf_wait(700);
    trx_dmr_burst_rx(rs.channel.rx_freq);
    lna(true, rs.channel.rx_freq);
    rf_wait(50);
}

/* The physical layer transmit's bursts (DMR in software, dmr_soft.c): its blocks are the 30 ms timeslots, even ones
   bursts (their 132 symbols from symbol 6 on), odd ones empty. The transmit interrupt can't wake this task (p25_phy.c),
   so the task follows its time stamps: after an empty block's interrupt the PA on for the next burst (if that block
   has data), after a burst's interrupt the PA off once the burst is out. The interrupts don't come 30 ms apart: measured
   (2026-11-20, a software receiver against known content, the first and the last 8 bits of the bursts) the burst's
   data starts ~28.2 ms after the empty block's interrupt and ends ~28.3 ms after its own (on at +27.5 left 12 % of the
   first 8 bits wrong, off at +28.0 20 % of the last 8). The FD6818 to the Tx frequency 4 ms ahead of the PA (it needs
   that long to settle: the first AMBE frame of the burst right 47 % tuned just before the PA, 72 % 1.5 ms ahead, ~90 %
   4 ms ahead, as the other two), the PA on at +26.9 ms (its ramp ~1.3 ms before the first symbol: the mask of
   10.2.3.1.1 allows 1.5 ms; at full 0.2 ms later); off at +28.7 ms, ~0.4 ms after the last symbol. The task sleeps
   until each time, busy waiting only its last ~1.2 ms (wait_until()). */
volatile uint32_t rf_gate_off_us = 28700, rf_gate_on_us = 26900; // tuned over the cable
volatile uint32_t rf_gate_tune_us = 4000; // the FD6818 to the Tx frequency this much before the PA (0: just before it)
static void rf_ask(uint32_t what);
static volatile bool gate_on;
static volatile bool pa_later;          // radio_phy_tx(): set up on the Tx frequency, the PA left to the gate
static volatile uint32_t gate_first_pa; // radio_phy_tx_gate_at(): the PA on for the first burst at this time
static bool wait_until(uint32_t at);

// Until systime_get_us() reaches at: asleep while more than a tick and a bit is left, the rest busy (at most ~1.2 ms).
// The RF task is the highest: busy waiting through the 4 ms of the FD6818's settling and 2 ms before each switch took
// so much of the CPU, with the AMBE encoder, that the UI task lost its events (a PTT release, 2026-11-20).
// false: the gating stopped meanwhile
static bool wait_until(uint32_t at)
{
    while ((int32_t)(at - systime_get_us()) > 1200) {
        if (!gate_on) {
            return false;
        }
        vTaskDelay(1);
    }
    while ((int32_t)(at - systime_get_us()) > 0) {
    }
    return true;
}

static void rf_gate(void)
{
    uint32_t done = UINT32_MAX;
    uint32_t first = gate_first_pa;
    if (first) { // the first burst's PA (set up without it: radio_phy_tx_pa_later())
        gate_first_pa = 0;
        if (!wait_until(first)) {
            return;
        }
        rf_pa_up();
    }
    while (gate_on && p25_phy_tx_active()) {
        uint32_t n = p25_phy_tx_isr_n, t = p25_phy_tx_isr_us, next = p25_phy_tx_isr_next;
        if ((n != p25_phy_tx_isr_n) || (n == UINT32_MAX) || (n == done)) {
            vTaskDelay(1);
            continue;
        }
        bool burst = !(n & 1);
        if (!burst && !next) { // no burst follows
            done = n;
            continue;
        }
        uint32_t lead = burst ? 0 : rf_gate_tune_us;
        uint32_t at = t + (burst ? rf_gate_off_us : rf_gate_on_us) - lead;
        if (!wait_until(at)) {
            break;
        }
        if (burst) {
            rf_off_now();
        } else if (lead) {
            lna(false, 0);
            trx_dmr_burst_tx(rs.channel.tx_freq);
            if (!wait_until(at + lead)) { // stopped before the PA: the FD6818 back to receive
                trx_dmr_burst_rx(rs.channel.rx_freq);
                lna(true, rs.channel.rx_freq);
                break;
            }
            rf_pa_up();
        } else {
            rf_on_wait(0);
        }
        done = n;
    }
}

// on: the physical layer transmit just started (its first block a burst, the PA on); off: as before (the PA as it is)
void radio_phy_tx_gate(bool on)
{
    gate_on = on;
    if (on) {
        rf_ask(RF_GATE);
    }
}

// The next radio_phy_tx(true): the transmitter set up on the Tx frequency, the PA off (repeater mode: no carrier before
// our first burst, in the other timeslot) ...
void radio_phy_tx_pa_later(bool on)
{
    pa_later = on;
}

// ... then the gate, its first burst's PA on at systime_get_us() pa_at
void radio_phy_tx_gate_at(uint32_t pa_at)
{
    gate_first_pa = pa_at;
    radio_phy_tx_gate(true);
}

static void rf_task_main(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t asked = 0;

        xTaskNotifyWait(0, UINT32_MAX, &asked, portMAX_DELAY);
        if (asked & RF_OFF) {
            rf_off();
        }
        if (asked & RF_RX) {
            // radioSetRx(), radioSetFrequency(rx), trxUpdateC7000Calibration()
            const factory_cal_t *cal = factory_cal_get();

            rf_off();
            modem_set_frequency_calibration(cal->valid, cal->osc_tune, cal->rx_adjust, factory_cal_mod_bias(rs.channel.tx_freq));
        }
        if (asked & RF_SETUP) {
            // radioSetTx(): the band of the Tx frequency, the modem's DMR calibration, the mic to the modem (vocoder)
            trx_set_frequency(rs.channel.tx_freq, false);
            trx_transmit_prepare();
            dmr_modem_calibration();
            gpio_output(PIN_MIC_MUT, false);
        }
        if ((asked & RF_SETUP) && pa_later) {
            pa_later = false;
            lna(false, 0);
            trx_dmr_burst_tx(rs.channel.tx_freq); // the PA by the gate (radio_phy_tx_gate_at())
        } else if (asked & (RF_SETUP | RF_ON)) {
            rf_on();
        }
        if (asked & RF_GATE) {
            rf_gate();
        }
    }
}

static void RAM_CODE rf_ask(uint32_t what)
{
    if (xPortIsInsideInterrupt()) {
        BaseType_t woken = pdFALSE;

        xTaskNotifyFromISR(rf_task, what, eSetBits, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xTaskNotify(rf_task, what, eSetBits);
    }
}

// The transmitter set up on the Tx frequency, then the RF on
void RAM_CODE radio_dmr_tx_on(void)
{
    rf_ask(RF_SETUP);
}

// Each burst: the RF on, or off with the FD6818 back on the Rx frequency meanwhile
void RAM_CODE radio_dmr_burst(bool on)
{
    rf_ask(on ? RF_ON : RF_OFF);
}

// Back to receive at the end
void RAM_CODE radio_dmr_rx_on(void)
{
    rf_ask(RF_RX);
}

// The physical layer transmit (p25_phy.c, a test for now): the transmitter on the channel's Tx frequency at the lowest
// power (50 mW), the PA on and kept on; off: back to receive. The RF set up rewrites WORK_MODE (dmr_modem_calibration()):
// the caller sets the modem up after it.
void radio_phy_tx(bool on, bool low_power)
{
    if (on) {
        dmr_drive = low_power ? level_drive(rs.channel.tx_freq, 0) : pa_drive(rs.channel.tx_freq);
        leds_red(true);
        rf_ask(RF_SETUP);
    } else {
        rf_ask(RF_RX);
        leds_red(false);
    }
}

bool radio_rf_on_air(void)
{
    return rf_on_air;
}

// The PA drive of the next DMR transmission
void rf_set_drive(uint16_t drive)
{
    dmr_drive = drive;
}

void rf_start(void)
{
    rf_task = xTaskCreateStatic(rf_task_main, "rf", sizeof(rf_task_stack) / sizeof(rf_task_stack[0]), NULL,
                                RF_TASK_PRIORITY, rf_task_stack, &rf_task_tcb);
}
