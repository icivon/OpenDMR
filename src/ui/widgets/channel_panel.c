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

#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "text.h"

// Layout of docs/ui-design.md 5.1, in LCD pixels. The zone line starts at y 44: the OpenGD77 header redraw clears and sends
// the LCD rows above 40.
#define ZONE_Y      44
#define NAME_Y      68
#define FREQUENCY_Y    112 // RX, then TX: roboto22, "MHz" at the baseline, the RX / TX labels right (VFO screen)
#define TX_FREQUENCY_Y 142
#define FREQ_CHIP_H    19
#define FREQ_CHIP_PAD  4
#define SEPARATOR_Y 174 // the VFO screen's line (vfo_screen.c LINE_Y)
#define LINE_Y      184 // the VFO screen's tone / talk group line (TONE_Y)
// The last heard callers under the talk group line, in the free area before the S meter (250): two rows, no title (the
// history icon on each row says what they are)
#define RECENT_Y          208
#define RECENT_ROW_HEIGHT 22

// The RX (green) / TX (red) label, the frequency after it, "MHz" at its baseline; both labels as wide, the
// frequencies one under the other
static void draw_frequency(int32_t y, const char *frequency, bool tx)
{
    const font_t *font = &font_roboto22;
    const char *label = text(tx ? TEXT_UI_TX : TEXT_UI_RX);
    int32_t rx_w = font_text_width(&font_roboto17_bold, text(TEXT_UI_RX));
    int32_t tx_w = font_text_width(&font_roboto17_bold, text(TEXT_UI_TX));
    int32_t chip_w = ((rx_w > tx_w) ? rx_w : tx_w) + (2 * FREQ_CHIP_PAD);
    int32_t cap = (font->ascent * 3) / 4;                                // the digits' height
    int32_t chip_y = y + font->ascent - cap - ((FREQ_CHIP_H - cap) / 2); // the label centred on the digits
    int32_t end;

    draw_fill_rect(WIDGET_MARGIN, chip_y, chip_w, FREQ_CHIP_H, (tx ? THEME_COLOUR_TX : THEME_COLOUR_RX));
    draw_text(WIDGET_MARGIN + (chip_w / 2), chip_y + ((FREQ_CHIP_H - font_roboto17_bold.line_height) / 2), label,
              &font_roboto17_bold, FONT_ALIGN_CENTER, (tx ? THEME_COLOUR_WHITE : THEME_COLOUR_BACKGROUND));
    end = font_draw_text(WIDGET_MARGIN + chip_w + 8, y, frequency, font, FONT_ALIGN_LEFT,
                         theme_colour(THEME_COLOUR_TEXT));
    if (frequency[0] != '-') {
        draw_text(end + 6, y + font->ascent - font_roboto17.ascent, "MHz", &font_roboto17, FONT_ALIGN_LEFT,
                  THEME_COLOUR_TEXT_LABEL);
    }
}

void channel_panel_draw(const channel_panel_data_t *data)
{
    const int32_t x = WIDGET_MARGIN;

    if (data->zone) {
        draw_text(x, ZONE_Y, data->zone, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }

    if (data->name) {
        draw_text(x, NAME_Y, data->name, &font_roboto32_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    }

    if (data->frequency) {
        draw_frequency(FREQUENCY_Y, data->frequency, false);
    }
    if (data->tx_frequency) {
        draw_frequency(TX_FREQUENCY_Y, data->tx_frequency, true);
    }

    if ((data->line_icon < ICONS_COUNT) || data->line_right) { // an FM channel without a tone: its squelch only
        const font_t *font = &font_roboto20;
        int32_t lx = x;

        draw_hline(x, SEPARATOR_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);

        if (data->line_icon < ICONS_COUNT) {
            lx += icons_draw(lx, LINE_Y + ((font->line_height - ICONS_SIZE) / 2) + 2, data->line_icon,
                             THEME_COLOUR_TEXT_LABEL) +
                  8;
        }

        if (data->line_text) {
            lx = font_draw_text(lx, LINE_Y, data->line_text, font, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_TEXT)) +
                 10;
        }

        if (data->line_detail) {
            draw_text(lx, LINE_Y + 2, data->line_detail, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        }

        if (data->line_right && data->line_right_squelch) {
            int32_t text_x = FB_WIDTH - WIDGET_MARGIN - font_text_width(&font_roboto17, data->line_right);

            icons_draw(text_x - 4 - ICONS_SQUELCH_WIDTH, LINE_Y + ((font->line_height - ICONS_SIZE) / 2) + 2,
                       ICONS_SQUELCH, THEME_COLOUR_TEXT_LABEL);
        }
        if (data->line_right) {
            draw_text(FB_WIDTH - WIDGET_MARGIN, LINE_Y + 2, data->line_right, &font_roboto17, FONT_ALIGN_RIGHT,
                      THEME_COLOUR_TEXT_LABEL);
        }
    }

    // The last heard callers: the line above is the channel's TX contact, not the caller's talk group, so each row
    // carries its own TG (or "PC"); the timeslot is already on the status bar
    for (uint8_t row = 0; (row < data->recent_count) && (row < CHANNEL_PANEL_RECENT); row++) {
        const char *name = data->recent[row];
        const char *detail = data->recent_detail[row];
        const char *when = data->recent_when[row];
        int32_t y = RECENT_Y + (row * RECENT_ROW_HEIGHT);
        int32_t lx = x;
        int32_t right_width;
        int32_t detail_width;
        int32_t name_width;

        if (!name) {
            continue;
        }
        lx += icons_draw(lx, y + ((font_roboto17.line_height - ICONS_SIZE) / 2) + 1, ICONS_HISTORY,
                         THEME_COLOUR_TEXT_LABEL) +
              8;
        right_width = when ? font_text_width(&font_roboto17, when) : 0;
        detail_width = detail ? (font_text_width(&font_roboto14, detail) + 8) : 0;
        name_width = (FB_WIDTH - WIDGET_MARGIN) - lx - detail_width - ((right_width > 0) ? (right_width + 8) : 0);
        if (name_width > 0) {
            draw_text_fitted(lx, y, name, &font_roboto17, name_width, THEME_COLOUR_TEXT);
        }
        if (detail) {
            draw_text(lx + name_width + 4, y + 3, detail, &font_roboto14, FONT_ALIGN_LEFT,
                      THEME_COLOUR_TEXT_SECOND);
        }
        if (when) {
            draw_text(FB_WIDTH - WIDGET_MARGIN, y, when, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);
        }
    }
}
