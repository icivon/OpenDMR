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
 * OpenDMR, screen: the number entry of the main firmware (uiTxTgPcContactOwnId.c, DM-32 drawScreen(),
 * docs/manual "号码输入"): what is entered and "# 换类型", the digits large, the contact of the number when the
 * codeplug has it; the contact selection is the menu list. # goes TG, PC, all call, contact, TG ... (#106: not the own
 * ID, which the channel's keys should not change; it has its own entry from the general options)
 *
 * The left key takes it: a TG, PC or the all call overrides the channel's contact (tx_target.c), a contact also sets
 * its timeslot. Then back home. The own ID from the general options is written to the codeplug (kept after power off),
 * then back there. On FM it is the DTMF entry: the digits, * and # type the code, LEFT deletes,
 * the left key sends it (tx_screen_send_dtmf()). On YSF it is the DG-ID entry (0 - 99): the current one shown, the
 * first digit typed replaces it, the left key gives it to the home screen (number_entry_open_dgid()).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "list_screen.h"
#include "codeplug.h"
#include "codeplug_text.h"
#include "contacts.h"
#include "sound.h"
#include "tx_target.h"
#include "dmr.h"
#include "tx_screen.h"
#include "number_entry.h"
#include "nxdn_id.h"
#include "extra_options.h"

#define DIGITS     CODEPLUG_CONTACT_ID_DIGITS
#define LABEL_Y    44
#define DIGITS_Y   70
#define HINT_Y     126
#define LINE_Y     160
#define NAME_SIZE  48

enum {
    ENTRY_TG,
    ENTRY_PC,
    ENTRY_ALL,               // the all call: 16777215, no digits typed
    ENTRY_CONTACT,
    ENTRIES,                 // the ones # goes through
    ENTRY_OWN_ID = ENTRIES,  // from the general options only
    ENTRY_DTMF,              // its own, # does not change it
    ENTRY_GATEWAY,           // the APRS-over-DMR gateway, its own too
    ENTRY_DGID,              // YSF: the DG-ID, its own too
};

#define DGID_DIGITS       2
#define NUMBER_ENTRY_DGID ((void *)6)

static struct {
    int type;
    bool own_only;
    bool nxdn;    // the own NXDN ID (own_only too)
    bool gateway; // the APRS-over-DMR gateway (own_only too)
    bool fresh;   // the DG-ID shown, nothing typed yet: a digit replaces it
    char digits[CODEPLUG_DTMF_CODE_LENGTH + 1]; // DIGITS of a number, or the DTMF code
    int length;
    int contact; // position in all the contacts (codeplug_contact_at())
    bool green_down;
    bool red_down;
} entry;

static codeplug_contact_t contact; // static: not on the UI task stack
static uint8_t dgid_current;
static void (*dgid_set)(uint8_t dgid);

static void set_digits(uint32_t value)
{
    snprintf(entry.digits, sizeof(entry.digits), "%lu", (unsigned long)value);
    entry.length = (int)strlen(entry.digits);
}

static bool read_contact(void)
{
    return codeplug_get_contact(codeplug_contact_at(entry.contact, -1), &contact);
}

static void enter(void *arg)
{
    if (arg == NULL) {
        return; // back from a screen it opened (none)
    }
    memset(&entry, 0, sizeof(entry));
    entry.nxdn = (arg == NUMBER_ENTRY_NXDN_ID);
    entry.gateway = (arg == NUMBER_ENTRY_APRS_GATEWAY);
    entry.own_only = (arg == NUMBER_ENTRY_OWN_ID) || entry.nxdn || entry.gateway;
    entry.type = entry.gateway ? ENTRY_GATEWAY
                              : (entry.own_only ? ENTRY_OWN_ID : ((arg == NUMBER_ENTRY_DTMF) ? ENTRY_DTMF : ENTRY_TG));
    if (arg == NUMBER_ENTRY_DGID) {
        entry.type = ENTRY_DGID;
        entry.own_only = true; // # doesn't change it
        entry.fresh = true;
        set_digits(dgid_current);
    } else if (entry.nxdn) {
        set_digits(nxdn_id_stored());
    } else if (entry.gateway) {
        set_digits(extra_options_aprs_dmr_gateway());
    } else if (entry.own_only) {
        set_digits(codeplug_get_dmr_id());
    }
}

static void next_type(void)
{
    entry.type = (entry.type + 1) % ENTRIES;
    entry.length = 0;
    entry.digits[0] = '\0';
    if (entry.type == ENTRY_CONTACT) {
        entry.contact = 0;
        if (codeplug_contact_count(-1) == 0) {
            entry.type = ENTRY_TG; // none to select
        }
    }
    if (entry.type == ENTRY_ALL) {
        set_digits(CODEPLUG_CONTACT_ID_MAX);
    }
}

void number_entry_open_dgid(uint8_t current, void (*set)(uint8_t dgid))
{
    dgid_current = (current <= CODEPLUG_YSF_DGID_MAX) ? current : 0;
    dgid_set = set;
    ui_push(&number_entry_screen, NUMBER_ENTRY_DGID);
}

static void take(void)
{
    uint32_t value = (uint32_t)strtoul(entry.digits, NULL, 10);

    if (entry.type == ENTRY_DGID) {
        if ((entry.length == 0) || (value > CODEPLUG_YSF_DGID_MAX) || (dgid_set == NULL)) {
            ui_key_beep(SOUND_ERROR);
            return;
        }
        ui_key_beep(SOUND_ACK);
        dgid_set((uint8_t)value);
        ui_pop();
        return;
    }
    if (entry.type == ENTRY_CONTACT) {
        if (!read_contact()) {
            ui_key_beep(SOUND_ERROR);
            return;
        }
        contacts_set_tx(contact.id, (contact.call_type == CODEPLUG_CALL_PRIVATE), codeplug_contact_timeslot(&contact));
        ui_key_beep(SOUND_ACK);
        ui_home();
        return;
    }
    if (entry.nxdn) { // 0: not set
        if (value > NXDN_ID_MAX) {
            ui_key_beep(SOUND_ERROR);
            return;
        }
        ui_key_beep(SOUND_ACK);
        nxdn_id_set((uint16_t)value);
        ui_pop();
        return;
    }
    if (entry.gateway) {
        if ((value == 0) || (value > 0xFFFFFFu)) { // a 24 bit DMR ID
            ui_key_beep(SOUND_ERROR);
            return;
        }
        ui_key_beep(SOUND_ACK);
        extra_options_set_aprs_dmr_gateway(value);
        ui_pop();
        return;
    }
    if ((value < CODEPLUG_CONTACT_ID_MIN) || (value > CODEPLUG_CONTACT_ID_MAX)) {
        ui_key_beep(SOUND_ERROR);
        return;
    }
    ui_key_beep(SOUND_ACK);
    if (entry.type == ENTRY_OWN_ID) {
        codeplug_set_dmr_id(value);
        dmr_set_own_id(value);
        if (entry.own_only) {
            ui_pop();
            return;
        }
    } else {
        contacts_set_tx(value, (entry.type == ENTRY_PC), 0);
    }
    ui_home();
}

static void step(int direction)
{
    if (entry.type == ENTRY_CONTACT) {
        int count = codeplug_contact_count(-1);

        entry.contact = (((entry.contact + direction) % count) + count) % count;
        if (entry.contact == 0) {
            ui_key_beep(SOUND_KEY_FIRST_ITEM);
        }
    } else if (entry.type == ENTRY_DGID) {
        int value = (int)strtoul(entry.digits, NULL, 10) + direction;

        set_digits((uint32_t)((value < 0) ? 0 : ((value > CODEPLUG_YSF_DGID_MAX) ? CODEPLUG_YSF_DGID_MAX : value)));
        entry.fresh = false;
    } else if (entry.length > 0) {
        uint32_t value = (uint32_t)strtoul(entry.digits, NULL, 10);

        if ((direction > 0) ? (value < CODEPLUG_CONTACT_ID_MAX) : (value > 1)) {
            set_digits(value + direction);
        }
    }
}

// The DTMF code: back home, then it is sent from there
static void send_dtmf(void)
{
    uint8_t code[CODEPLUG_DTMF_CODE_LENGTH];

    memset(code, 0xFF, sizeof(code));
    for (int i = 0; i < entry.length; i++) {
        char c = entry.digits[i];

        code[i] = (c == '*') ? 14 : ((c == '#') ? 15 : (uint8_t)(c - '0'));
    }
    ui_pop();
    if (!tx_screen_send_dtmf(code)) {
        sound_play(SOUND_ERROR);
    }
}

static bool handle_dtmf(const ui_event_t *ev)
{
    int digit = ui_key_digit(ev->key);
    char c = (digit >= 0) ? (char)('0' + digit) : ((ev->key == UI_KEY_STAR) ? '*' : ((ev->key == UI_KEY_HASH) ? '#' : 0));

    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_GREEN) && entry.green_down) {
        entry.green_down = false;
        if (entry.length == 0) {
            ui_key_beep(SOUND_ERROR);
            return false;
        }
        send_dtmf();
        return false;
    }
    if (((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) && (ev->key == UI_KEY_LEFT) &&
        (entry.length > 0)) {
        entry.digits[--entry.length] = '\0';
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && c && (entry.length < CODEPLUG_DTMF_CODE_LENGTH)) {
        entry.digits[entry.length++] = c;
        entry.digits[entry.length] = '\0';
        return true;
    }
    return false;
}

static bool handle(const ui_event_t *ev)
{
    bool press = (ev->type == UI_EVENT_KEY_PRESS);
    bool repeat = press || (ev->type == UI_EVENT_KEY_REPEAT);
    int digit = ui_key_digit(ev->key);

    if (ev->type == UI_EVENT_ROTARY) {
        step(ev->steps);
        return true;
    }
    if (press) {
        entry.green_down |= (ev->key == UI_KEY_GREEN);
        entry.red_down |= (ev->key == UI_KEY_RED);
    }
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && entry.red_down) {
        ui_pop();
        return false;
    }
    if (entry.type == ENTRY_DTMF) {
        return handle_dtmf(ev);
    }
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_GREEN) && entry.green_down) {
        entry.green_down = false;
        take();
        return true;
    }
    if (press && (ev->key == UI_KEY_HASH) && !entry.own_only) {
        next_type();
        return true;
    }
    if (repeat && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN)) && (entry.type != ENTRY_ALL)) {
        // the contacts as the menu lists (down: the next one), the numbers up: + 1
        step(((ev->key == UI_KEY_UP) != (entry.type == ENTRY_CONTACT)) ? 1 : -1);
        return true;
    }
    if ((entry.type == ENTRY_CONTACT) || (entry.type == ENTRY_ALL)) {
        return false; // nothing typed
    }
    if (repeat && (ev->key == UI_KEY_LEFT) && (entry.length > 0)) {
        entry.digits[--entry.length] = '\0';
        entry.fresh = false;
        return true;
    }
    if ((entry.type == ENTRY_DGID) && press && (digit >= 0)) {
        if (entry.fresh || ((entry.length == 1) && (entry.digits[0] == '0'))) {
            entry.length = 0; // the one shown, or a lone 0: replaced
        }
        entry.fresh = false;
        if (entry.length < DGID_DIGITS) {
            entry.digits[entry.length++] = (char)('0' + digit);
            entry.digits[entry.length] = '\0';
        }
        return true;
    }
    if (press && (digit >= 0) && (entry.length < DIGITS) && ((entry.length > 0) || (digit > 0))) {
        entry.digits[entry.length++] = (char)('0' + digit);
        entry.digits[entry.length] = '\0';
        return true;
    }
    return false;
}

static void draw_contacts(void)
{
    int count = codeplug_contact_count(-1);
    char position[24];

    snprintf(position, sizeof(position), "%d/%d", entry.contact + 1, count);
    list_view_draw_title(text(TEXT_UI_SELECT_CONTACT), position);
    for (int row = 0; row < LIST_VIEW_ROWS; row++) {
        int at = list_row_index(row, entry.contact, count);
        char name[NAME_SIZE];
        char value[24];

        if (at < 0) {
            break;
        }
        if (!codeplug_get_contact(codeplug_contact_at(at, -1), &contact)) {
            continue;
        }
        codeplug_text_name(contact.name, CODEPLUG_NAME_LENGTH, name, sizeof(name));

        list_view_row_t data = {.name = name, .value = value, .selected = (at == entry.contact)};

        if (contact.call_type == CODEPLUG_CALL_PRIVATE) {
            data.icon = ICONS_CONTACT;
            snprintf(value, sizeof(value), "%s", text(TEXT_PC));
        } else {
            data.icon = ICONS_TALKGROUP;
            snprintf(value, sizeof(value), "%s %lu", text(TEXT_TG), (unsigned long)contact.id);
        }
        list_view_draw_row(row, &data);
    }
    bottom_bar_draw_soft_keys(text(TEXT_UI_OK), text(TEXT_UI_BACK));
}

static void draw(void)
{
    static const text_id_t labels[ENTRY_OWN_ID + 1] = {TEXT_TG_ENTRY, TEXT_PC_ENTRY, TEXT_ALL_CALL, TEXT_CONTACT,
                                                       TEXT_USER_DMR_ID};
    status_bar_data_t status = {0};
    digit_entry_t shown = {.max = DIGITS};
    char buffer[NAME_SIZE + 16];
    int32_t x = WIDGET_MARGIN;

    if (entry.type == ENTRY_CONTACT) {
        draw_contacts();
        return;
    }
    status_bar_draw(&status);
    if (entry.type == ENTRY_DTMF) {
        int32_t end;

        draw_text(x, LABEL_Y, text(TEXT_DTMF_ENTRY), &font_roboto17_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        end = font_draw_text(x, DIGITS_Y, entry.digits, &font_roboto32_bold, FONT_ALIGN_LEFT,
                             theme_colour(THEME_COLOUR_TEXT));
        if (entry.length < CODEPLUG_DTMF_CODE_LENGTH) {
            draw_fill_rect(end, DIGITS_Y + font_roboto32_bold.line_height - 2, 16, 3, THEME_COLOUR_ACCENT); // the cursor
        }
        bottom_bar_draw_soft_keys(text(TEXT_UI_OK), text(TEXT_UI_BACK));
        return;
    }
    if (entry.type == ENTRY_DGID) {
        shown.max = DGID_DIGITS;
        draw_text(x, LABEL_Y, "DG-ID", &font_roboto17_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        memcpy(shown.digits, entry.digits, DGID_DIGITS);
        shown.count = (uint8_t)entry.length;
        digit_entry_draw(&shown, x, DIGITS_Y);
        snprintf(buffer, sizeof(buffer), "%s: %u   (0 - %u)", text(TEXT_UI_NOW), (unsigned)dgid_current,
                 (unsigned)CODEPLUG_YSF_DGID_MAX);
        draw_text(x, HINT_Y, buffer, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        draw_hline(x, LINE_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);
        draw_text(x, LINE_Y + 10, text(TEXT_UI_UP_DOWN_STEP), &font_roboto17, FONT_ALIGN_LEFT,
                  THEME_COLOUR_TEXT_SECOND);
        bottom_bar_draw_soft_keys(text(TEXT_UI_OK), text(TEXT_UI_BACK));
        return;
    }
    text_id_t label = entry.nxdn ? TEXT_NXDN_ID
                                 : ((entry.type == ENTRY_GATEWAY) ? TEXT_APRS_GATEWAY : labels[entry.type]);

    draw_text(x, LABEL_Y, text(label), &font_roboto17_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    if (!entry.own_only) {
        draw_text(FB_WIDTH - WIDGET_MARGIN, LABEL_Y, text(TEXT_UI_HASH_TYPE), &font_roboto17, FONT_ALIGN_RIGHT,
                  THEME_COLOUR_TEXT_LABEL);
    }
    memcpy(shown.digits, entry.digits, DIGITS);
    shown.count = (uint8_t)entry.length;
    digit_entry_draw(&shown, x, DIGITS_Y);

    if ((entry.type == ENTRY_OWN_ID) || (entry.type == ENTRY_GATEWAY)) {
        snprintf(buffer, sizeof(buffer), "%s: %lu", text(TEXT_UI_NOW),
                 (unsigned long)(entry.gateway ? extra_options_aprs_dmr_gateway()
                                               : (entry.nxdn ? nxdn_id_get() : codeplug_get_dmr_id())));
        draw_text(x, HINT_Y, buffer, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        draw_hline(x, LINE_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);
        draw_text(x, LINE_Y + 10, text(TEXT_UI_OK_SAVE_ID), &font_roboto17, FONT_ALIGN_LEFT,
                  THEME_COLOUR_TEXT_SECOND);
    } else if (entry.type != ENTRY_ALL) {
        draw_text(x, HINT_Y, text(TEXT_UI_UP_DOWN_STEP), &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);

        // The name of the number when it is a contact of the codeplug
        bool pc = (entry.type == ENTRY_PC);
        uint32_t id = (uint32_t)strtoul(entry.digits, NULL, 10);
        int index = (entry.length > 0) ? codeplug_contact_find(id, (pc ? CODEPLUG_CALL_PRIVATE : CODEPLUG_CALL_GROUP), 0)
                                       : 0;

        if ((index != 0) && codeplug_get_contact(index, &contact)) {
            char name[NAME_SIZE];
            int32_t lx;

            draw_hline(x, LINE_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);
            snprintf(buffer, sizeof(buffer), "%s %lu", text(pc ? TEXT_PC : TEXT_TG), (unsigned long)id);
            lx = x + icons_draw(x, LINE_Y + 15, (pc ? ICONS_CONTACT : ICONS_TALKGROUP), THEME_COLOUR_TEXT_LABEL) + 8;
            lx = font_draw_text(lx, LINE_Y + 10, buffer, &font_roboto20, FONT_ALIGN_LEFT,
                                theme_colour(THEME_COLOUR_TEXT)) + 10;
            codeplug_text_name(contact.name, CODEPLUG_NAME_LENGTH, name, sizeof(name));
            draw_text(lx, LINE_Y + 12, name, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        }
    }
    bottom_bar_draw_soft_keys(text(TEXT_UI_OK), text(TEXT_UI_BACK));
}

const ui_screen_t number_entry_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
};
