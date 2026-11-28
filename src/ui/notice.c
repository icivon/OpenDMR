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
 * OpenDMR, UI: the notice bubble (ui_notify() in ui.h, shown by the UI task over the screen on top), as the toasts of
 * the phone apps: a solid card with round corners in the middle of the screen, a large icon of its kind, the text
 * centred under it. The card is the colour of the menus' selected row (THEME_COLOUR_ACCENT), the icon and the text
 * the selected row's text colour (THEME_COLOUR_ON_ACCENT): the kind's own colours (transmit, receive, warning) are
 * lost on the accent of several themes, so the icon's shape tells the kind. A shadow under it on a light theme.
 */

#include <stddef.h>
#include <stdio.h>
#include "framebuffer.h"
#include "theme.h"
#include "draw.h"
#include "fonts.h"
#include "ui.h"

#define WIDTH_MIN   150
#define WIDTH_MAX   (FB_WIDTH - 12)
#define PADDING_X   14
#define PADDING_TOP 14
#define PADDING_BOT 12
#define ICON_H      30
#define ICON_GAP    8
#define LINE_GAP    2
#define RADIUS      10
#define SHADOW      3
#define TEXT_SIZE   96

// A filled rectangle with round corners
static void round_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, theme_colour_t colour)
{
    uint16_t c = theme_colour(colour);

    fb_fill(x + r, y, x + w - r, y + h, c);
    fb_fill(x, y + r, x + w, y + h - r, c);
    draw_fill_circle(x + r, y + r, 4 * r, colour);
    draw_fill_circle(x + w - r - 1, y + r, 4 * r, colour);
    draw_fill_circle(x + r, y + h - r - 1, 4 * r, colour);
    draw_fill_circle(x + w - r - 1, y + h - r - 1, 4 * r, colour);
}

// A line 4 pixels wide (draw_line()'s thick one is 2 x 2: four of them)
static void wide_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, theme_colour_t colour)
{
    for (int32_t dy = -1; dy <= 1; dy += 2) {
        for (int32_t dx = -1; dx <= 1; dx += 2) {
            draw_line(x0 + dx, y0 + dy, x1 + dx, y1 + dy, colour, true);
        }
    }
}

// The sides of a ring round (cx, cy), radius r, 3 pixels wide: "( )", the waves of the transmit icon
static void wave(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    for (int32_t y = -r - 1; y <= r + 1; y++) {
        for (int32_t x = -r - 1; x <= r + 1; x++) {
            int32_t d2 = (x * x) + (y * y);
            int32_t ax = (x < 0) ? -x : x;
            int32_t ay = (y < 0) ? -y : y;

            if (((4 * d2) >= (((2 * r) - 3) * ((2 * r) - 3))) && ((4 * d2) <= (((2 * r) + 3) * ((2 * r) + 3))) &&
                ((ax * 5) > (ay * 6))) {
                fb_fill(cx + x, cy + y, cx + x + 1, cy + y + 1, c);
            }
        }
    }
}

// The kind's icon, centred on (cx, cy), about 30 pixels high, its lines 4 pixels wide
static void icon(ui_notice_kind_t kind, int32_t cx, int32_t cy, theme_colour_t colour)
{
    switch (kind) {
        case UI_NOTICE_UNLOCK: // a tick, as UI_NOTICE_OK (the padlock is on the status bar; locked: the i below)
        case UI_NOTICE_OK: // a tick
            wide_line(cx - 12, cy + 1, cx - 4, cy + 9, colour);
            wide_line(cx - 4, cy + 9, cx + 12, cy - 9, colour);
            break;
        case UI_NOTICE_ERROR: // a cross
            wide_line(cx - 10, cy - 10, cx + 10, cy + 10, colour);
            wide_line(cx - 10, cy + 10, cx + 10, cy - 10, colour);
            break;
        case UI_NOTICE_TX: { // an antenna: the mast, its tip, two waves each side
            uint16_t c = theme_colour(colour);

            fb_fill(cx - 2, cy - 4, cx + 2, cy + 15, c);
            draw_fill_circle(cx, cy - 6, 4 * 4, colour);
            wave(cx, cy - 6, 9, c);
            wave(cx, cy - 6, 15, c);
            break;
        }
        case UI_NOTICE_MAIL: { // an envelope: its outline, the flap
            uint16_t c = theme_colour(colour);

            fb_fill(cx - 17, cy - 12, cx + 17, cy - 8, c);
            fb_fill(cx - 17, cy + 8, cx + 17, cy + 12, c);
            fb_fill(cx - 17, cy - 12, cx - 13, cy + 12, c);
            fb_fill(cx + 13, cy - 12, cx + 17, cy + 12, c);
            wide_line(cx - 14, cy - 9, cx, cy + 3, colour);
            wide_line(cx, cy + 3, cx + 14, cy - 9, colour);
            break;
        }
        default: // an i in a ring (UI_NOTICE_INFO, UI_NOTICE_LOCK)
            draw_ring(cx, cy, 4 * 13, 4 * 3, colour);
            draw_fill_circle(cx, cy - 6, 4 * 2, colour);
            fb_fill(cx - 2, cy - 2, cx + 2, cy + 8, theme_colour(colour));
            break;
    }
}

void ui_notice_draw(text_id_t line1, text_id_t line2, const char *line2_text, ui_notice_kind_t kind)
{
    const font_t *font = &font_roboto20;
    bool dark = theme_dark();
    theme_colour_t card = THEME_COLOUR_ACCENT;
    theme_colour_t ink = THEME_COLOUR_ON_ACCENT;
    const char *t1 = text(line1);
    const char *t2 = line2_text ? line2_text : (line2 < TEXT_COUNT) ? text(line2) : NULL;
    char joined[TEXT_SIZE];
    const char *lines[2] = {t1, NULL};
    int32_t widest;

    // one line ("APRS Sent"); two when that is too wide
    if (t2) {
        snprintf(joined, sizeof(joined), "%s %s", t1, t2);
        if (font_text_width(font, joined) <= (WIDTH_MAX - (2 * PADDING_X))) {
            lines[0] = joined;
        } else {
            lines[1] = t2;
        }
    }
    widest = font_text_width(font, lines[0]);
    if (lines[1] && (font_text_width(font, lines[1]) > widest)) {
        widest = font_text_width(font, lines[1]);
    }

    int32_t w = widest + (2 * PADDING_X);
    int32_t h = PADDING_TOP + ICON_H + ICON_GAP + font->line_height + PADDING_BOT;

    if (lines[1]) {
        h += LINE_GAP + font->line_height;
    }
    w = (w < WIDTH_MIN) ? WIDTH_MIN : ((w > WIDTH_MAX) ? WIDTH_MAX : w);

    int32_t x = (FB_WIDTH - w) / 2;
    int32_t y = (FB_HEIGHT - h) / 2;
    int32_t inner = w - (2 * PADDING_X);

    if (!dark) {
        round_rect(x + 1, y + SHADOW, w, h, RADIUS, THEME_COLOUR_OFF); // the shadow
    }
    round_rect(x, y, w, h, RADIUS, card);
    icon(kind, x + (w / 2), y + PADDING_TOP + (ICON_H / 2), ink);

    int32_t ty = y + PADDING_TOP + ICON_H + ICON_GAP;

    for (int i = 0; (i < 2) && lines[i]; i++) {
        int32_t lw = font_text_width(font, lines[i]);

        lw = (lw > inner) ? inner : lw;
        draw_text_fitted(x + ((w - lw) / 2), ty, lines[i], font, inner, ink);
        ty += font->line_height + LINE_GAP;
    }
}
