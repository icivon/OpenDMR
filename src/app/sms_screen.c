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
 * OpenDMR, app: the text messages' screens (sms_screen.h), as the user chose them from the mockups (2026-11-24):
 *
 * - the main menu's "Messages" with the unread count; its menu: Inbox ("2 new"), Outbox, Write message, Quick texts
 * - a box: two-line rows as the last heard (who, when / the outbox's state, the text's start; unread with the mail
 *   icon); the left key reads, SK2 + the left key deletes the selected one (asked first; "Delete" on the soft key
 *   while SK2 is held, as reading)
 * - reading: who as the title, the mode, the call and when under it, the text wrapped (UP / DOWN scroll); the left key
 *   replies (the inbox: writing to the same) or sends it again (the outbox: as it was, to the same, on the channel
 *   now), SK2 + the left key deletes (asked first)
 * - writing: the To row (DMR: the ID typed, # a talk group or a radio; M17: the callsign typed, empty: all) and the
 *   text (text_entry.c's keys; # the quick texts); UP / DOWN between the two; the left key sends on the channel's mode
 *   (sms.c), the result as a notice; the title counts the characters (SMS_CHARS)
 * - the quick texts (written by the CPS): picked into the text, or from the menu into a new message
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "widgets.h"
#include "fonts.h"
#include "draw.h"
#include "codeplug.h"
#include "radio.h"
#include "datetime.h"
#include "dmr_ids.h"
#include "sound.h"
#include "sms.h"
#include "dmr_sms.h"
#include "text_entry.h"
#include "message_box.h"
#include "tx_target.h"
#include "lock_screen.h"
#include "sms_screen.h"

#define BODY_TOP    LIST_VIEW_TOP
#define BODY_BOTTOM WIDGET_BOTTOM_BAR_TOP
#define LINES_MAX   48          // wrapped lines of a message kept
#define ENTRY_MAX   255         // octets typed (text_entry.c)

static sms_t msg; // the message shown (512 octets: not on the UI task's stack)

/* ---- shared ---- */

// who: the inbox's sender, the outbox's destination (a talk group "TG 9")
static void peer_of(int box, const sms_t *m, char *out, int size)
{
    if (m->mode == SMS_MODE_M17) {
        snprintf(out, (size_t)size, "%s", m->peer[0] ? m->peer : text(TEXT_SMS_ALL));
        return;
    }
    uint32_t id = (box == SMS_INBOX) ? m->src : m->dst;
    if (m->group && (box == SMS_OUTBOX)) {
        snprintf(out, (size_t)size, "%s %lu", text(TEXT_TG), (unsigned long)id);
        return;
    }
    dmr_ids_caller_text(id, out, (size_t)size);
    if (!out[0]) {
        snprintf(out, (size_t)size, "%lu", (unsigned long)id);
    }
}

// how long ago: "now", "5m", "3h", "2d" (no clock: "")
static void ago(uint32_t when, char *out, int size)
{
    uint32_t now = datetime_now();
    out[0] = 0;
    if ((when == 0) || (now < when) || !datetime_is_valid()) {
        return;
    }
    uint32_t s = now - when;
    if (s < 60) {
        snprintf(out, (size_t)size, "<1m");
    } else if (s < 3600) {
        snprintf(out, (size_t)size, "%lum", (unsigned long)(s / 60));
    } else if (s < 86400) {
        snprintf(out, (size_t)size, "%luh", (unsigned long)(s / 3600));
    } else {
        snprintf(out, (size_t)size, "%lud", (unsigned long)(s / 86400));
    }
}

static const text_id_t status_text[] = {TEXT_SMS_SENDING, TEXT_SMS_SENT, TEXT_SMS_DELIVERED, TEXT_SMS_FAILED};

// Word wrap: the starts and lengths (octets) of the lines of t within width; their count
static int wrap(const char *t, const font_t *font, int32_t width, uint16_t *start, uint16_t *len, int max)
{
    int n = 0;
    const char *line = t;
    while (*line && (n < max)) {
        const char *p = line, *space = NULL, *end = line;
        char buf[SMS_TEXT_MAX + 1];
        for (;;) {
            const char *q = p;
            uint32_t c = font_utf8_next(&q);
            if ((c == 0) || (c == '\n')) {
                end = p;
                break;
            }
            size_t k = (size_t)(q - line);
            memcpy(buf, line, k);
            buf[k] = 0;
            if ((font_text_width(font, buf) > width) && (p > line)) {
                end = (space && (space > line)) ? space : p; // at the last space, else before this character
                break;
            }
            if (c == ' ') {
                space = p;
            }
            p = q;
        }
        start[n] = (uint16_t)(line - t);
        len[n] = (uint16_t)(end - line);
        n++;
        line = end;
        if ((*line == '\n') || (*line == ' ')) {
            line++;
        }
    }
    return n;
}

// Deleting: SK2 + the left key, in a box (the selected message) or reading one, asked first. While SK2 is held the left
// soft key says "Delete".
static bool sk2_held;
static int delete_box, delete_index;
static bool delete_pops; // reading: back to the box once deleted

static void deleted(bool yes)
{
    if (yes && sms_delete(delete_box, delete_index) && delete_pops) {
        ui_pop();
    }
}

static void ask_delete(int box, int index, bool pops)
{
    static message_box_t question;

    delete_box = box;
    delete_index = index;
    delete_pops = pops;
    question = (message_box_t){
        .title = text(TEXT_SMS),
        .line1 = text(TEXT_SMS_DELETE_QM),
        .buttons = MESSAGE_BOX_YES_NO,
        .answered = deleted,
    };
    ui_push(&message_box_screen, (void *)&question);
}

// SK2 pressed or released: true (the soft key's text changes)
static bool sk2_changed(const ui_event_t *ev)
{
    if ((ev->key != UI_KEY_SK2) || ((ev->type != UI_EVENT_KEY_PRESS) && (ev->type != UI_EVENT_KEY_RELEASE))) {
        return false;
    }
    sk2_held = (ev->type == UI_EVENT_KEY_PRESS);
    return true;
}

/* ---- a box ---- */

static int box_first;

static void box_clamp(int box, int32_t *sel)
{
    int count = sms_count(box);
    if (*sel >= count) {
        *sel = (count > 0) ? (count - 1) : 0;
    }
    if (*sel < box_first) {
        box_first = *sel;
    } else if (*sel >= (box_first + LIST_VIEW_ROWS2)) {
        box_first = *sel - (LIST_VIEW_ROWS2 - 1);
    }
}

static const ui_screen_t read_screen;

// the read screen's arg: the box and the index, + 1 (NULL is "back on top")
static void *read_arg(int box, int index)
{
    return (void *)(uintptr_t)(((box << 8) | (index & 0xFF)) + 1);
}

static int box_of(void)
{
    return (int)(uintptr_t)ui_arg();
}

static void box_enter(void *arg)
{
    (void)arg;
    sk2_held = false;
    box_first = 0;
    box_clamp(box_of(), ui_state());
}

static bool box_handle(const ui_event_t *ev)
{
    int box = box_of();
    int32_t *sel = ui_state();
    int count = sms_count(box);
    if (ev->type == UI_EVENT_DATA) {
        box_clamp(box, sel);
        return true; // a message came, a state changed
    }
    if (sk2_changed(ev)) {
        return true;
    }
    if (((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) && (count > 0)) {
        if (ev->key == UI_KEY_DOWN) {
            *sel = (*sel < (count - 1)) ? (*sel + 1) : *sel;
            box_clamp(box, sel);
            return true;
        }
        if (ev->key == UI_KEY_UP) {
            *sel = (*sel > 0) ? (*sel - 1) : 0;
            box_clamp(box, sel);
            return true;
        }
        if ((ev->key == UI_KEY_GREEN) && (ev->type == UI_EVENT_KEY_PRESS)) {
            if (ev->modifiers & UI_MODIFIER_SK2) {
                ask_delete(box, (int)*sel, false);
            } else {
                ui_push(&read_screen, read_arg(box, (int)*sel));
            }
            return false;
        }
    }
    list_handle_back(ev);
    return false;
}

static void box_draw(void)
{
    int box = box_of();
    int32_t sel = *ui_state();
    int count = sms_count(box);
    char position[16], who[48], when[16], sub[64];
    snprintf(position, sizeof(position), "%d/%d", (int)(sel + 1), count);
    list_view_draw_title(text((box == SMS_INBOX) ? TEXT_SMS_INBOX : TEXT_SMS_OUTBOX), (count > 0) ? position : NULL);
    bottom_bar_draw_soft_keys(text((sk2_held && (count > 0)) ? TEXT_UI_DELETE : TEXT_SMS_READ), text(TEXT_UI_BACK));
    if (count == 0) {
        list_view_row_t row = {.icon = ICONS_COUNT, .name = text(TEXT_LIST_EMPTY), .dimmed = true};
        list_view_draw_row(0, &row);
        return;
    }
    for (int r = 0; r < LIST_VIEW_ROWS2; r++) {
        if (!sms_get(box, box_first + r, &msg)) {
            break;
        }
        peer_of(box, &msg, who, sizeof(who));
        if (box == SMS_OUTBOX) {
            snprintf(when, sizeof(when), "%s", text(status_text[msg.status & 3]));
        } else {
            ago(msg.time, when, sizeof(when));
        }
        // the text's start, one line; the mode before it (M17)
        snprintf(sub, sizeof(sub), "%s%.56s", (msg.mode == SMS_MODE_M17) ? "M17  " : "", msg.text);
        for (char *c = sub; *c; c++) {
            if (*c == '\n') {
                *c = ' ';
            }
        }
        list_view_row_t row = {
            .icon = ((box == SMS_INBOX) && (msg.unread == SMS_UNREAD)) ? ICONS_MAIL : ICONS_COUNT,
            .name = who,
            .value = when,
            .sub = sub,
            .selected = ((box_first + r) == sel),
        };
        list_view_draw_row2(LIST_VIEW_TOP + (r * LIST_VIEW_ROW2_HEIGHT), &row);
    }
}

static const ui_screen_t box_screen = {
    .enter = box_enter,
    .handle = box_handle,
    .draw = box_draw,
};

/* ---- reading ---- */

static uint16_t line_start[LINES_MAX], line_len[LINES_MAX];
static int lines, read_first, read_box, read_index;
static const ui_screen_t compose_screen;

typedef struct {
    uint8_t mode;
    bool group;
    uint32_t id;
    char peer[24];
    char text[ENTRY_MAX + 1];
} compose_t;
static compose_t compose; // writing (kept while the screen is under another: the quick texts)

static const font_t *body_font(void)
{
    return &font_roboto20;
}

static int read_rows(void)
{
    return (BODY_BOTTOM - BODY_TOP - 28) / body_font()->line_height;
}

static void read_enter(void *arg)
{
    sk2_held = false;
    if (arg) { // pushed (back on top: NULL, the same message)
        uintptr_t a = (uintptr_t)arg - 1;
        read_box = (int)(a >> 8);
        read_index = (int)(a & 0xFF);
        read_first = 0;
    }
    if (!sms_get(read_box, read_index, &msg)) {
        lines = 0;
        return;
    }
    if (read_box == SMS_INBOX) {
        sms_mark_read(read_index);
    }
    lines = wrap(msg.text, body_font(), FB_WIDTH - (2 * WIDGET_MARGIN), line_start, line_len, LINES_MAX);
}

static bool read_handle(const ui_event_t *ev)
{
    bool sk2 = (ev->modifiers & UI_MODIFIER_SK2) != 0;
    if (sk2_changed(ev)) {
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) {
        if (ev->key == UI_KEY_DOWN) {
            if ((read_first + read_rows()) < lines) {
                read_first++;
            }
            return true;
        }
        if (ev->key == UI_KEY_UP) {
            read_first = (read_first > 0) ? (read_first - 1) : 0;
            return true;
        }
        if ((ev->key == UI_KEY_GREEN) && (ev->type == UI_EVENT_KEY_PRESS)) {
            if (sk2) {
                ask_delete(read_box, read_index, true);
                return false;
            }
            if (read_box == SMS_OUTBOX) { // sent again, as it was
                if (sms_send(msg.dst, msg.group, msg.peer, msg.text)) {
                    ui_notify(TEXT_SMS, TEXT_SMS_SENDING, UI_NOTICE_TX, 3000);
                    ui_pop(); // back to the outbox: the new one on top
                } else {
                    ui_key_beep(SOUND_ERROR);
                    ui_notify(TEXT_SMS, TEXT_SMS_NO_CHANNEL, UI_NOTICE_ERROR, 3000);
                }
                return false;
            }
            memset(&compose, 0, sizeof(compose)); // a reply: to the same
            compose.mode = msg.mode;
            compose.group = msg.group;
            compose.id = (read_box == SMS_INBOX) ? (msg.group ? msg.dst : msg.src) : msg.dst;
            snprintf(compose.peer, sizeof(compose.peer), "%s", msg.peer);
            ui_push(&compose_screen, (void *)&compose);
            return false;
        }
    }
    list_handle_back(ev);
    return false;
}

static void read_draw(void)
{
    char who[48], info[64], when[16];
    peer_of(read_box, &msg, who, sizeof(who));
    list_view_draw_title(who, NULL);
    text_id_t left = (read_box == SMS_OUTBOX) ? TEXT_SMS_RESEND : TEXT_SMS_REPLY;

    bottom_bar_draw_soft_keys(text(sk2_held ? TEXT_UI_DELETE : left), text(TEXT_UI_BACK));
    if (!lines && !msg.text[0]) {
        return;
    }
    ago(msg.time, when, sizeof(when));
    if (msg.mode == SMS_MODE_M17) {
        snprintf(info, sizeof(info), "M17  %s", when);
    } else {
        uint32_t id = (read_box == SMS_INBOX) ? msg.src : msg.dst;
        snprintf(info, sizeof(info), "DMR  %s %lu  %s", msg.group ? text(TEXT_TG) : text(TEXT_PRIVATE_CALL),
                 (unsigned long)(msg.group ? msg.dst : id), when);
    }
    if (read_box == SMS_OUTBOX) {
        size_t n = strlen(info);
        snprintf(&info[n], sizeof(info) - n, "  %s", text(status_text[msg.status & 3]));
    }
    draw_text_fitted(WIDGET_MARGIN, BODY_TOP + 2, info, &font_roboto14, FB_WIDTH - (2 * WIDGET_MARGIN),
                     THEME_COLOUR_TEXT_LABEL);
    fb_fill(WIDGET_MARGIN, BODY_TOP + 24, FB_WIDTH - WIDGET_MARGIN, BODY_TOP + 25, theme_colour(THEME_COLOUR_LINE));
    int32_t y = BODY_TOP + 28;
    for (int i = read_first; (i < lines) && (i < (read_first + read_rows())); i++) {
        char line[SMS_TEXT_MAX + 1];
        memcpy(line, &msg.text[line_start[i]], line_len[i]);
        line[line_len[i]] = 0;
        draw_text(WIDGET_MARGIN, y, line, body_font(), FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        y += body_font()->line_height;
    }
}

static const ui_screen_t read_screen = {
    .enter = read_enter,
    .handle = read_handle,
    .draw = read_draw,
};

/* ---- writing ---- */

static text_entry_t entry_to, entry_text;
static char to_text[24];
static int focus; // 0 the To row, 1 the text
static bool compose_dmr;
static const ui_pick_list_t presets_pick;

// a message's destination from the To row
static void to_from_entry(void)
{
    if (compose_dmr) {
        compose.id = (uint32_t)strtoul(to_text, NULL, 10);
    } else {
        snprintf(compose.peer, sizeof(compose.peer), "%s", to_text);
    }
}

static void compose_enter(void *arg)
{
    if (!arg) { // back on top (from the quick texts)
        return;
    }
    if (arg != (void *)&compose) { // a new message: to the channel's own (DMR: its TG or private call; M17: all)
        memset(&compose, 0, sizeof(compose));
        codeplug_channel_t channel;
        if (radio_get_channel(&channel)) {
            compose.mode = (channel.mode == CODEPLUG_MODE_M17) ? SMS_MODE_M17 : SMS_MODE_DMR;
            if (channel.mode == CODEPLUG_MODE_DMR) {
                tx_target_t t;
                tx_target_get(&channel, &t);
                compose.group = (t.id >> 24) != TX_TARGET_PC_FLAG;
                compose.id = t.id & 0xFFFFFF;
            }
        }
        if (arg != (void *)1) { // a quick text picked from the menu
            snprintf(compose.text, sizeof(compose.text), "%s", (const char *)arg);
        }
    }
    codeplug_channel_t channel;
    compose_dmr = !radio_get_channel(&channel) || (channel.mode != CODEPLUG_MODE_M17);
    if (compose_dmr) {
        snprintf(to_text, sizeof(to_text), "%lu", (unsigned long)compose.id);
        if (compose.id == 0) {
            to_text[0] = 0;
        }
    } else {
        snprintf(to_text, sizeof(to_text), "%s", compose.peer);
    }
    text_entry_start(&entry_to, to_text, compose_dmr ? 8 : 9, compose_dmr);
    text_entry_start(&entry_text, compose.text, ENTRY_MAX, false);
    focus = (compose.text[0] || (compose_dmr ? compose.id : 1)) ? 1 : 0;
}

static void send(void)
{
    text_entry_commit(&entry_text);
    text_entry_commit(&entry_to);
    to_from_entry();
    if (!compose.text[0]) {
        ui_key_beep(SOUND_ERROR);
        return;
    }
    if (!sms_send(compose.id, compose.group, compose.peer, compose.text)) {
        ui_key_beep(SOUND_ERROR);
        ui_notify(TEXT_SMS, TEXT_SMS_NO_CHANNEL, UI_NOTICE_ERROR, 3000);
        return;
    }
    ui_notify(TEXT_SMS, TEXT_SMS_SENDING, UI_NOTICE_TX, 3000);
    memset(compose.text, 0, sizeof(compose.text));
    ui_pop();
}

// a quick text into the text at its cursor
static void insert(const char *s)
{
    size_t n = strlen(s), len = strlen(compose.text);
    if ((len + n) > ENTRY_MAX) {
        return;
    }
    text_entry_commit(&entry_text);
    memmove(&compose.text[entry_text.pos + n], &compose.text[entry_text.pos], len - entry_text.pos + 1);
    memcpy(&compose.text[entry_text.pos], s, n);
    entry_text.pos = (uint8_t)(entry_text.pos + n);
}

static bool compose_handle(const ui_event_t *ev)
{
    text_entry_t *e = focus ? &entry_text : &entry_to;
    bool press = (ev->type == UI_EVENT_KEY_PRESS);
    if (press && (ev->key == UI_KEY_GREEN) && !(ev->modifiers & UI_MODIFIER_SK2)) {
        send();
        return true;
    }
    if (press && (ev->key == UI_KEY_HASH)) {
        if (focus) {
            ui_push(&pick_list_screen, (void *)&presets_pick);
        } else if (compose_dmr) {
            compose.group = !compose.group; // a talk group or a radio
        }
        return true;
    }
    if (press && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
        text_entry_commit(e);
        focus = (ev->key == UI_KEY_DOWN) ? 1 : 0;
        return true;
    }
    if (text_entry_handle(e, ev)) {
        if (!focus) {
            to_from_entry();
        } else if ((dmr_sms_units(compose.text) > SMS_CHARS) && (entry_text.pos > 0)) {
            // one too many: the character typed out again (a whole UTF-8 one before the cursor)
            size_t at = entry_text.pos;
            do {
                at--;
            } while ((at > 0) && ((compose.text[at] & 0xC0) == 0x80));
            memmove(&compose.text[at], &compose.text[entry_text.pos], strlen(&compose.text[entry_text.pos]) + 1);
            entry_text.pos = (uint8_t)at;
            entry_text.tap_key = -1;
            ui_key_beep(SOUND_ERROR);
        }
        return true;
    }
    list_handle_back(ev);
    return false;
}

static void compose_draw(void)
{
    char title[24], to[48];
    int units = dmr_sms_units(compose.text);
    snprintf(title, sizeof(title), "%d/%d", units, SMS_CHARS);
    list_view_draw_title(text(TEXT_SMS_COMPOSE), title);
    bottom_bar_draw_soft_keys(text(TEXT_SMS_SEND), text(TEXT_UI_BACK));
    // the To row
    if (compose_dmr) {
        snprintf(to, sizeof(to), "%s %s", text(compose.group ? TEXT_TG : TEXT_PRIVATE_CALL), to_text);
    } else {
        snprintf(to, sizeof(to), "%s", to_text[0] ? to_text : text(TEXT_SMS_ALL));
    }
    list_view_row_t row = {.icon = ICONS_COUNT, .name = text(TEXT_SMS_TO), .value = to, .selected = (focus == 0)};
    if (focus == 0) {
        row.cursor = text_entry_cursor(&entry_to, (int)strlen(to) - (int)strlen(to_text));
    }
    list_view_draw_row(0, &row);
    // the text, wrapped, with its cursor ("|" where it is)
    static char shown[ENTRY_MAX + 2];
    size_t pos = focus ? entry_text.pos : strlen(compose.text);
    snprintf(shown, sizeof(shown), "%.*s%s%s", (int)pos, compose.text, focus ? "|" : "", &compose.text[pos]);
    int n = wrap(shown, body_font(), FB_WIDTH - (2 * WIDGET_MARGIN) - 8, line_start, line_len, LINES_MAX);
    int32_t top = LIST_VIEW_TOP + LIST_VIEW_ROW_HEIGHT + 6, bottom = BODY_BOTTOM - 22;
    int rows = (bottom - top - 8) / body_font()->line_height;
    int cursor_line = 0;
    for (int i = 0; i < n; i++) {
        if ((line_start[i] <= pos) && (pos <= (size_t)(line_start[i] + line_len[i]))) {
            cursor_line = i;
        }
    }
    int first = (cursor_line >= rows) ? (cursor_line - rows + 1) : 0;
    theme_colour_t frame = focus ? THEME_COLOUR_ACCENT : THEME_COLOUR_LINE;
    draw_hline(WIDGET_MARGIN - 4, top, FB_WIDTH - (2 * WIDGET_MARGIN) + 8, frame);
    draw_hline(WIDGET_MARGIN - 4, bottom, FB_WIDTH - (2 * WIDGET_MARGIN) + 8, frame);
    draw_fill_rect(WIDGET_MARGIN - 4, top, 1, bottom - top, frame);
    draw_fill_rect(FB_WIDTH - WIDGET_MARGIN + 3, top, 1, bottom - top + 1, frame);
    int32_t y = top + 4;
    for (int i = first; (i < n) && (i < (first + rows)); i++) {
        char line[ENTRY_MAX + 2];
        memcpy(line, &shown[line_start[i]], line_len[i]);
        line[line_len[i]] = 0;
        draw_text(WIDGET_MARGIN, y, line, body_font(), FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        y += body_font()->line_height;
    }
    draw_text_fitted(WIDGET_MARGIN, bottom + 2, text(TEXT_SMS_HINT_KEYS), &font_roboto14,
                     FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_TEXT_LABEL);
}

static const ui_screen_t compose_screen = {
    .enter = compose_enter,
    .handle = compose_handle,
    .draw = compose_draw,
};

/* ---- the quick texts ---- */

static int presets_count(void)
{
    return sms_presets();
}

static void presets_name(int index, char *buffer, size_t size)
{
    if (!sms_preset(index, buffer, (int)size)) {
        buffer[0] = 0;
    }
}

static char picked[SMS_PRESET_SIZE];

static void presets_picked(int index)
{
    if (!sms_preset(index, picked, sizeof(picked))) {
        return;
    }
    ui_pop(); // the quick texts
    if (ui_top_screen() == &compose_screen) {
        insert(picked);
    } else { // from the menu: a new message with it
        ui_push(&compose_screen, (void *)picked);
    }
}

static const ui_pick_list_t presets_pick = {
    .title = TEXT_SMS_PRESETS,
    .icon = ICONS_COUNT,
    .count = presets_count,
    .name = presets_name,
    .pick = presets_picked,
    .action = TEXT_SMS_INSERT,
};

/* ---- the menus ---- */

static void inbox_format(char *buffer, size_t size)
{
    int unread = sms_unread();
    if (unread) {
        snprintf(buffer, size, "%d %s", unread, text(TEXT_SMS_UNREAD));
    } else {
        snprintf(buffer, size, "%d", sms_count(SMS_INBOX));
    }
}

static void inbox_open(void)
{
    ui_push(&box_screen, (void *)(uintptr_t)SMS_INBOX);
}

static void outbox_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%d", sms_count(SMS_OUTBOX));
}

static void outbox_open(void)
{
    ui_push(&box_screen, (void *)(uintptr_t)SMS_OUTBOX);
}

static void write_open(void)
{
    ui_push(&compose_screen, (void *)1);
}

static void presets_open(void)
{
    ui_push(&pick_list_screen, (void *)&presets_pick);
}

static const ui_custom_t inbox_item = {.format = inbox_format, .select = inbox_open};
static const ui_custom_t outbox_item = {.format = outbox_format, .select = outbox_open};
// list.c calls format() of every custom item: an empty value
static void no_value(char *buffer, size_t size)
{
    (void)size;
    buffer[0] = 0;
}

static const ui_custom_t write_item = {.format = no_value, .select = write_open};
static const ui_custom_t presets_item = {.format = no_value, .select = presets_open};

static const ui_item_t sms_menu_items[] = {
    {TEXT_SMS_INBOX, ICONS_MAIL, UI_ITEM_CUSTOM, &inbox_item, NULL},
    {TEXT_SMS_OUTBOX, ICONS_TX, UI_ITEM_CUSTOM, &outbox_item, NULL},
    {TEXT_SMS_COMPOSE, ICONS_CONTACT_ADD, UI_ITEM_CUSTOM, &write_item, NULL},
    {TEXT_SMS_PRESETS, ICONS_CHANNEL, UI_ITEM_CUSTOM, &presets_item, NULL},
};

static const ui_list_t sms_menu = {
    .title = TEXT_SMS,
    .items = sms_menu_items,
    .count = sizeof(sms_menu_items) / sizeof(sms_menu_items[0]),
};

static void entry_format(char *buffer, size_t size)
{
    int unread = sms_unread();
    if (unread) {
        snprintf(buffer, size, "%d", unread);
    } else {
        buffer[0] = 0;
    }
}

static void entry_open(void)
{
    ui_push(&list_screen, (void *)&sms_menu);
}

const ui_custom_t sms_menu_entry = {.format = entry_format, .select = entry_open};

// The new message's notice: its left key reads it (the newest of the inbox); not while the keypad is locked
static void notice_read(void)
{
    if (!lock_screen_is_keypad_locked() && (sms_count(SMS_INBOX) > 0)) {
        ui_push(&read_screen, read_arg(SMS_INBOX, 0));
    }
}

void sms_screen_init(void)
{
    ui_set_notice_action(UI_NOTICE_MAIL, notice_read, TEXT_SMS_READ);
}

static const text_id_t formats[DMR_SMS_FORMATS] = {TEXT_SMS_HYTERA, TEXT_SMS_MOTOROLA, TEXT_SMS_DMR};

static void format_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text(formats[sms_dmr_format() % DMR_SMS_FORMATS]));
}

static void format_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    sms_set_dmr_format((uint8_t)((sms_dmr_format() + DMR_SMS_FORMATS + direction) % DMR_SMS_FORMATS));
}

const ui_custom_t sms_format_item = {.format = format_format, .change = format_change};
