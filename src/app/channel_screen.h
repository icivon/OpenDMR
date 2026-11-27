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

#ifndef DM32_CHANNEL_SCREEN_H
#define DM32_CHANNEL_SCREEN_H

#include "ui.h"

// The channel screen (docs/ui-design.md 5.1): the home screen in channel mode
extern const ui_screen_t channel_screen;
extern volatile int channel_screen_channel; // the channel number shown

// The channel shown, for the channel details to change in place (the radio follows after each event); written to the
// codeplug by them. The changes made with the keys are dropped first: they edit the channel as it is. NULL: none
codeplug_channel_t *channel_screen_edit(void);

// The changes made with the keys on the channel screen (squelch, power, mode, bandwidth, the TG and timeslot picked)
// dropped: the channel as it is in the codeplug again (the VFO shown); only the power (Power To All)
void channel_screen_forget_changes(void);
void channel_screen_forget_power(void);

// A channel locked out of the scans for good: its Zone Skip, or All Skip in the all channels zone (scan lockouts)
bool channel_screen_lock_out(int number);
int channel_screen_scanning(void); // 0 no scan, 1 scanning, 2 standing on a signal (the test interface)
// The dual watch: channel number shown (in the zone in use when it is there, else in all channels); false: not in use
bool channel_screen_select(int number);
// The dual watch, back on a channel path: the squelch it had there, as if changed with the keys (for now, never
// written; codeplug sql: 0 the band default, 1 .. 21)
void channel_screen_set_squelch(uint8_t sql);
int channel_screen_typed(void); // the channel number being typed (its position in the zone), 0: none

#endif
