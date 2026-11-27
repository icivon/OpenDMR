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
 * OpenDMR, app: the settings menus' OK / Cancel (config_snapshot.h). Every store the menus change, copied and put
 * back: a store a menu item gets to change must be in here, or Cancel keeps that change.
 */

#include <stdbool.h>
#include "settings.h"
#include "sound.h"
#include "extra_options.h"
#include "language.h"
#include "security.h"
#include "display.h"
#include "gps.h"
#include "power.h"
#include "list_screen.h"
#include "config_snapshot.h"

static struct {
    bool taken;
    settings_t settings;
    sound_tx_tones_t tx_tones;
    extra_options_block_t extra;
    uint8_t language;
    uint8_t theme;
    bool boot_pin;
    bool lock_pin;
    bool places_kept;
} copy;

static bool gps_on(uint8_t mode)
{
    return (mode == GPS_MODE_ON) || (mode == GPS_MODE_ON_NMEA);
}

void config_snapshot_take(void)
{
    copy.settings = settings;
    copy.tx_tones = sound_tx_tones;
    extra_options_get_block(&copy.extra);
    copy.language = language_get();
    copy.theme = display_theme_choice();
    copy.boot_pin = security_boot_pin_on();
    copy.lock_pin = security_lock_pin_on();
    copy.places_kept = ui_list_places_kept();
    copy.taken = true;
}

// The language first: it sets a bit of the settings (the Chinese one), which the settings put back after it have as
// they were
void config_snapshot_restore(void)
{
    bool gps_was = gps_on(settings.gps);

    if (!copy.taken) {
        return;
    }
    copy.taken = false;
    language_set(copy.language);
    settings = copy.settings;
    settings_changed();
    (void)settings_save_if_changed();
    sound_tx_tones = copy.tx_tones;
    sound_tx_tones_changed();
    sound_tx_tones_save();
    extra_options_set_block(&copy.extra);
    if (display_theme_choice() != copy.theme) {
        display_theme_choose(copy.theme);
    }
    if (security_boot_pin_on() != copy.boot_pin) {
        security_set_boot_pin(copy.boot_pin);
    }
    if (security_lock_pin_on() != copy.lock_pin) {
        security_set_lock_pin(copy.lock_pin);
    }
    ui_list_places_keep(copy.places_kept);
    if (gps_on(settings.gps) != gps_was) {
        gps_set_enabled(gps_on(settings.gps));
    }
    display_apply_brightness();
}

void config_snapshot_drop(void)
{
    copy.taken = false;
}

// Switched off in a menu with changes not confirmed: they go, as Cancel does (restore() saves what it puts back, so
// the other savers' order doesn't matter)
static void restore_at_power_off(void)
{
    config_snapshot_restore();
}

void config_snapshot_init(void)
{
    ui_list_settings_take = config_snapshot_take;
    ui_list_settings_restore = config_snapshot_restore;
    ui_list_settings_drop = config_snapshot_drop;
    power_off_save_register(restore_at_power_off);
}
