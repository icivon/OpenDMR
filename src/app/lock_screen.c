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
 * OpenDMR, screen: the keypad and PTT lock (uiLockScreen.c; the key filter of applicationMain.c, the * and #
 * keys of the main menu in menuDisplayMenuList.c).
 *
 * The lock only exists on the home screens: locking goes home, as the original pops to the root menu. While the
 * keypad is locked every key, the side buttons and the rotary are taken; a key press shows the lock page a moment,
 * the unlock keys (key settings, SK1 + # by default; not SK2 + * of the main firmware: on the DM-32 a held SK2 hides
 * the column of *, keys.c) unlock both. The PTT still works unless the PTT is locked too. The page takes the keys
 * itself, so the unlock works while it shows. The auto lock: idle.c asks the home screens (UI_DATA_AUTOLOCK). TODO:
 * the voice prompts. With the lock PIN (security.h) the lock takes the PTT too, the unlock keys ask for the PIN
 * (pin_screen.c), and the radio starts locked (unless the power-on PIN was asked); the reception goes on. Then any key
 * (not the PTT, not a side key) asks for the PIN, a digit as its first one. Then the lock page stays as the lock
 * screen (the time, no channel) until the PIN unlocks, and the VOX is off (radio.c).
 */

#include <stddef.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "text.h"
#include "datetime.h"
#include "security.h"
#include "pin_screen.h"
#include "lock_screen.h"
#include "cable_link.h"
#include "sound.h"
#include "keymap.h"
#include "ui.h"

#define SHOW_MS  1000
#define NOTICE_MS 1500 // the lock / unlock bubbles (ui_notify())
#define CLOCK_MS 5000 // the lock screen's clock
#define TIME_Y   70
#define TEXT_Y   150
#define CABLE_ACTIVE_MS 2000 // the CPS or an update on the cable: no lock (it would cut them off half way)

static bool keypad_locked = false;
static bool ptt_locked = false;
static const ui_screen_t lock_page;

// Locked with the lock PIN: the lock page stays (the lock screen) instead of showing a moment
static bool lock_screen_stays(void)
{
    return keypad_locked && security_lock_pin_on();
}

// The state as a bubble over the home screen (#193: the page showed a second, then nothing told it was locked; the
// status bar's padlock now does): locked and how to unlock, the PTT locked, or unlocked
static void show(void)
{
    status_bar_set_locked(keypad_locked);
    if (!lock_screen_stays()) {
        if (keypad_locked) {
            ui_notify(TEXT_LOCKED, TEXT_UI_UNLOCK_HINT, UI_NOTICE_LOCK, NOTICE_MS);
        } else if (ptt_locked) {
            ui_notify(TEXT_LOCKED, TEXT_PTT, UI_NOTICE_LOCK, NOTICE_MS);
        } else {
            ui_notify(TEXT_UNLOCKED, TEXT_COUNT, UI_NOTICE_UNLOCK, NOTICE_MS);
        }
        ui_redraw(); // the status bar's padlock
        return;
    }
    if (ui_top_screen() == &lock_page) {
        if (!lock_screen_stays()) {
            ui_timer_start(SHOW_MS); // shows already: again from now
        }
        ui_redraw();
    } else {
        ui_push(&lock_page, (void *)&lock_page);
    }
}

void lock_screen_lock(bool keypad, bool ptt)
{
    if (keypad && !keypad_locked && cable_link_used_within(CABLE_ACTIVE_MS)) {
        sound_play(SOUND_ERROR); // the cable link would stop answering (security.h)
        return;
    }
    keypad_locked = keypad;
    ptt_locked = ptt || (keypad && security_lock_pin_on());
    security_set_locked(keypad_locked);
    ui_home();
    show();
}

void lock_screen_unlock(void)
{
    keypad_locked = false;
    ptt_locked = false;
    security_set_locked(false);
    show(); // "Unlocked"
}

const ui_screen_t *lock_screen_start(void)
{
    if (security_lock_pin_on() && !security_boot_pin_on()) {
        keypad_locked = true;
        ptt_locked = true;
        security_set_locked(true);
        status_bar_set_locked(true);
        return &lock_page;
    }
    return NULL;
}

bool lock_screen_is_keypad_locked(void)
{
    return keypad_locked;
}

bool lock_screen_is_ptt_locked(void)
{
    return ptt_locked;
}

static bool is_key(const ui_event_t *ev)
{
    return (ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_LONG) || (ev->type == UI_EVENT_KEY_REPEAT) ||
           (ev->type == UI_EVENT_KEY_RELEASE);
}

bool lock_screen_filter(const ui_event_t *ev)
{
    if (ev->type == UI_EVENT_ROTARY) {
        return keypad_locked;
    }
    if (!is_key(ev)) {
        return false;
    }
    if (ev->key == UI_KEY_PTT) {
        if (ptt_locked && (ev->type == UI_EVENT_KEY_PRESS)) {
            show();
        }
        return ptt_locked;
    }
    if (!keypad_locked) {
        return false;
    }
    // The unlock keys (key settings; SK1 + # by default). keymap.c sees every key while locked, so a side key held
    // with them doesn't count as pressed alone when released (it would run its own action, as the lock again)
    if (keymap_action(ev, KEYMAP_SCREEN_LOCKED) == KEYMAP_UNLOCK) {
        if (security_lock_pin_on()) {
            pin_screen_unlock(-1); // right: lock_screen_unlock()
        } else {
            lock_screen_unlock();
        }
        return true;
    }
    if (ev->type != UI_EVENT_KEY_PRESS) {
        return true;
    }
    if (lock_screen_stays() && (ev->key != UI_KEY_SK1) && (ev->key != UI_KEY_SK2)) {
        pin_screen_unlock(ui_key_digit(ev->key)); // the lock PIN: any key asks for it
    } else if ((ev->key != UI_KEY_SK1) && (ev->key != UI_KEY_SK2)) {
        show(); // a side key alone may be the first half of the unlock
    }
    return true;
}

static void page_enter(void *arg)
{
    (void)arg;
    ui_timer_start(lock_screen_stays() ? CLOCK_MS : SHOW_MS); // also back on top
}

static bool page_handle(const ui_event_t *ev)
{
    if (ev->type == UI_EVENT_TIMER) {
        if (lock_screen_stays()) {
            ui_timer_start(CLOCK_MS);
            return true;
        }
        ui_pop();
        return false;
    }
    if (lock_screen_stays()) {
        (void)lock_screen_filter(ev);
        return true; // the clock
    }
    if (lock_screen_filter(ev)) {
        return false; // show() restarted the timer and redraws
    }
    if (is_key(ev) || (ev->type == UI_EVENT_ROTARY)) {
        ui_pop(); // unlocked (or only the PTT locked): the key is for the home screen
    }
    return false;
}

// The lock screen: only the time, "Locked" and how to unlock; no status bar, nothing of the channel
static void lock_screen_draw(void)
{
    char time[16];

    if (datetime_format_local_time(time, sizeof(time))) {
        draw_text(FB_WIDTH / 2, TIME_Y, time, &font_roboto44_bold, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT);
    }
    draw_text(FB_WIDTH / 2, TEXT_Y, text(TEXT_LOCKED), &font_roboto20, FONT_ALIGN_CENTER, THEME_COLOUR_WARNING);
    draw_text(FB_WIDTH / 2, TEXT_Y + 30, text(TEXT_UI_ANY_KEY_UNLOCK), &font_roboto17, FONT_ALIGN_CENTER,
              THEME_COLOUR_TEXT_LABEL);
}

static void page_draw(void)
{
    bool locked = keypad_locked || ptt_locked;

    if (lock_screen_stays()) {
        lock_screen_draw();
        return;
    }
    list_view_draw_title(text(TEXT_KEYPAD), NULL);
    if (!locked) {
        list_view_draw_text(text(TEXT_UNLOCKED), NULL, THEME_COLOUR_RX);
    } else {
        list_view_draw_text(text(TEXT_LOCKED), (keypad_locked ? text(TEXT_UI_UNLOCK_HINT) : text(TEXT_PTT)),
                            THEME_COLOUR_WARNING);
    }
    bottom_bar_draw_soft_keys(NULL, NULL);
}

static const ui_screen_t lock_page = {
    .enter = page_enter,
    .handle = page_handle,
    .draw = page_draw,
};
