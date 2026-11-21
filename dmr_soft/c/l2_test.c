// src/services/dmr_l2.c on the hotspot test transmitter's frame file (PC test):
//   l2_test frames.txt     (lines "DMR1 33 <hex>": a call's bursts)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmr_l2.h"

static int hex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) {
            return 0;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    // RS (12,9): Table B.18's first row (message 01 00 .. 00: parity 1C BC FD) is a codeword
    uint8_t rs[12] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0x1C, 0xBC, 0xFD};
    CHECK(dmr_full_lc_check(rs, 0), "RS row 1");
    uint8_t rs8[12] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0x0E, 0x38, 0x40};
    CHECK(dmr_full_lc_check(rs8, 0), "RS row 9");
    rs8[4] ^= 0x5A;
    CHECK(dmr_full_lc_check(rs8, 0) && (rs8[4] == 0), "RS one byte corrected");

    FILE *f = fopen(argv[1], "r");
    char line[512];
    dmr_emb_lc e;
    dmr_emb_lc_reset(&e);
    int n = 0;
    while (f && fgets(line, sizeof(line), f)) {
        uint8_t b[33];
        if (strncmp(line, "DMR", 3) || !hex(strchr(line + 5, ' ') + 1, b, 33)) {
            continue;
        }
        int k = dmr_sync_kind(dmr_burst_middle(b), 4);
        printf("burst %d: sync %d", n, k);
        if (DMR_SYNC_IS_DATA(k)) {
            uint8_t cc, dt, lc[12];
            bool st = dmr_slot_type(b, &cc, &dt);
            bool ok = dmr_bptc196(b, lc);
            uint32_t mask = (dt == DMR_DT_VOICE_LC_HEADER) ? DMR_MASK_VOICE_LC_HEADER : DMR_MASK_TERMINATOR_LC;
            bool rsok = ok && dmr_full_lc_check(lc, mask);
            dmr_lc_t p;
            dmr_lc_parse(lc, &p);
            printf(" slot type %d cc %d dt %d bptc %d rs %d: flco %d fid %d opt %02X dst %u src %u", st, cc, dt, ok,
                   rsok, p.flco, p.fid, p.options, p.dst, p.src);
            CHECK(st && ok && rsok, "data burst %d", n);
            // a few bit errors: still decoded the same
            uint8_t bad[33], lc2[12];
            memcpy(bad, b, 33);
            int flips[] = {3, 40, 77, 170, 201, 250};
            for (int i = 0; i < 6; i++) {
                bad[flips[i] / 8] ^= (uint8_t)(0x80 >> (flips[i] % 8));
            }
            bool ok2 = dmr_bptc196(bad, lc2) && dmr_full_lc_check(lc2, mask) && !memcmp(lc2, lc, 9);
            printf(" (6 bit errors: %s)", ok2 ? "same" : "NOT");
            CHECK(ok2, "BPTC with errors, burst %d", n);
        } else if (k == DMR_SYNC_NONE) {
            uint8_t cc, pi, lcss, frag[4], lc[9];
            bool ok = dmr_emb(b, &cc, &pi, &lcss);
            dmr_embedded(b, frag);
            printf(" emb %d cc %d pi %d lcss %d", ok, cc, pi, lcss);
            CHECK(ok, "EMB burst %d", n);
            if (dmr_emb_lc_add(&e, lcss, frag, lc)) {
                dmr_lc_t p;
                dmr_lc_parse(lc, &p);
                printf(" -> embedded LC flco %d fid %d opt %02X dst %u src %u", p.flco, p.fid, p.options, p.dst, p.src);
            }
        }
        if (k == DMR_SYNC_NONE || DMR_SYNC_IS_VOICE(k)) {
            uint8_t v[3][9];
            dmr_voice_frames(b, v);
            printf(" ambe %02X%02X%02X..", v[2][0], v[2][1], v[2][2]);
        }
        printf("\n");
        n++;
    }
    // transmit: each burst of the file made again from its content, byte for byte
    if (f) {
        rewind(f);
    }
    uint8_t lc9[9] = {0}, frags[4][4];
    int vn = 0;
    n = 0;
    while (f && fgets(line, sizeof(line), f)) {
        uint8_t b[33], made[33];
        if (strncmp(line, "DMR", 3) || !hex(strchr(line + 5, ' ') + 1, b, 33)) {
            continue;
        }
        int k = dmr_sync_kind(dmr_burst_middle(b), 4);
        uint8_t v[3][9];
        dmr_voice_frames(b, v);
        if (DMR_SYNC_IS_DATA(k)) {
            uint8_t cc, dt, info[12], full[12];
            dmr_slot_type(b, &cc, &dt);
            dmr_bptc196(b, info);
            memcpy(lc9, info, 9);
            dmr_lc_t p;
            dmr_lc_parse(lc9, &p);
            uint8_t lc2[9];
            dmr_lc_make(lc2, p.flco, p.options, p.dst, p.src);
            CHECK(!memcmp(lc2, lc9, 9), "LC made again, burst %d", n);
            dmr_full_lc_make(lc9, (dt == DMR_DT_VOICE_LC_HEADER) ? DMR_MASK_VOICE_LC_HEADER : DMR_MASK_TERMINATOR_LC, full);
            dmr_burst_data(made, full, cc, dt, k);
            dmr_emb_lc_make(lc9, frags);
            vn = 0;
        } else if (k != DMR_SYNC_NONE) {
            dmr_burst_voice_sync(made, v, k);
            vn = 0;
        } else {
            static const uint8_t lcss[5] = {DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST, DMR_LCSS_SINGLE};
            static const uint8_t zero[4] = {0, 0, 0, 0};
            uint8_t cc, pi, ls;
            dmr_emb(b, &cc, &pi, &ls);
            dmr_burst_voice_emb(made, v, cc, pi, lcss[vn], (vn < 4) ? frags[vn] : zero);
            vn++;
        }
        bool same = !memcmp(made, b, 33);
        printf("made again: burst %d %s\n", n, same ? "same" : "DIFFERENT");
        CHECK(same, "burst %d made again", n);
        n++;
    }
    // RS (12,9): Table B.18's first row made
    uint8_t m1[9] = {1, 0, 0, 0, 0, 0, 0, 0, 0}, out[12];
    dmr_full_lc_make(m1, 0, out);
    CHECK((out[9] == 0x1C) && (out[10] == 0xBC) && (out[11] == 0xFD), "RS parity of row 1");
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
