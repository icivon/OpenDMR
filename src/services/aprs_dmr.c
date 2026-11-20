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
 * The LRRP report and the DMR data packet formats of this file are ported from RadioFoundry
 * (https://github.com/gzalo/RadioFoundry), Copyright (c) The RadioFoundry Authors, and are used under the MIT
 * License. The full notice is in the NOTICE file at the root of the project. Keep it with the ported code.
 */

/*
 * OpenDMR, service: APRS over DMR (aprs_dmr.h). The RadioFoundry LRRP report is an MBXML document inside an IPv4/UDP
 * packet (port 4001) in an unconfirmed DMR data transmission: two preamble CSBKs, a data header, then Rate 1/2 blocks
 * with the CRC-32. The HR-C7000 adds the FEC and the sync, so these are the 10 / 12 octets written to its TX RAM.
 */

#include <string.h>
#include "aprs_dmr.h"

#define LRRP_PORT             4001u
#define IP_PROTOCOL_UDP       17u
#define DMR_CAI_INDIVIDUAL    0x0Cu
#define IP_ID_DEFAULT         0u

#define DPF_UNCONFIRMED       0x02u
#define SAP_IP                0x04u
#define CSBK_OPCODE_PREAMBLE  0x3Du

#define DOC_TRIGGERED_REPORT  0x0Du
#define TOKEN_REQUEST_ID      0x22u
#define TOKEN_POSITION_RADIUS 0x51u
#define TOKEN_SPEED_HOR       0x6Cu
#define TOKEN_DIRECTION       0x56u

#define PREAMBLES             2
#define RATE_12_BYTES         12
#define LRRP_MAX_DOCUMENT     64
#define PACKET_MAX            128

static const uint8_t REPORT_ID[4] = {0, 0, 0, 1};

static uint16_t ip_identification = IP_ID_DEFAULT;
static uint8_t ip_packet[PACKET_MAX];
static uint8_t packet_data[PACKET_MAX];

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_id(uint8_t *p, uint32_t id)
{
    p[0] = (uint8_t)(id >> 16);
    p[1] = (uint8_t)(id >> 8);
    p[2] = (uint8_t)id;
}

static int put_uintvar(uint8_t *out, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;

    do {
        tmp[n++] = (uint8_t)(v & 0x7Fu);
        v >>= 7;
    } while (v != 0);
    for (int i = 0; i < n; i++) {
        out[i] = (uint8_t)(tmp[n - 1 - i] | ((i < (n - 1)) ? 0x80u : 0u));
    }
    return n;
}

// 10-byte CSBK / data header (the HR_C7000 computes the CRC-16 and fills bytes 10..11)
static void add_burst(aprs_dmr_tx_t *tx, uint8_t type, const uint8_t *payload)
{
    aprs_dmr_burst_t *burst = &tx->burst[tx->count++];

    burst->type = type;
    burst->length = (type == APRS_DMR_BURST_RATE_1_2) ? RATE_12_BYTES : 10;
    memset(burst->payload, 0, sizeof(burst->payload));
    memcpy(burst->payload, payload, burst->length);
}

// ETSI B.3.9: pairs of octets are taken least significant first, polynomial 0x04C11DB7, remainder zero
static uint32_t crc32_dmr(const uint8_t *data, int length)
{
    uint32_t crc = 0;
    int padded = (length + 1) & ~1;

    for (int i = 0; i < padded; i++) {
        int index = i ^ 1;
        crc ^= (uint32_t)((index < length) ? data[index] : 0) << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000UL) ? ((crc << 1) ^ 0x04C11DB7UL) : (crc << 1);
        }
    }
    return crc;
}

static uint16_t ones_complement_sum(uint32_t sum, const uint8_t *data, int length)
{
    for (int i = 0; i < length; i += 2) {
        sum += (uint32_t)((data[i] << 8) | (((i + 1) < length) ? data[i + 1] : 0));
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)sum;
}

// 1e-7 degrees -> degrees and 1/10000 minute, as RadioFoundry's lrrpPositionFromE7()
static void degrees_minutes(int32_t e7, uint8_t *degrees, uint32_t *minutes)
{
    uint32_t value = (e7 < 0) ? (uint32_t)(-(int64_t)e7) : (uint32_t)e7;

    *degrees = (uint8_t)(value / 10000000u);
    *minutes = (((value % 10000000u) * 60u) + 500u) / 1000u;
    if (*minutes >= 600000u) {
        // Rounded up to a whole degree: the minutes go back to 0, not 59.9999 with the degree added.
        *minutes = 0;
        if (*degrees < 255u) {
            (*degrees)++;
        }
    }
}

// The position token's raw 32 bit coordinates (2^31 per 90 degrees latitude, 2^32 per 360 longitude)
static uint32_t raw_coordinate(int32_t e7, bool latitude)
{
    uint8_t degrees;
    uint32_t minutes;
    uint64_t n;
    uint64_t magnitude;
    uint32_t raw;

    degrees_minutes(e7, &degrees, &minutes);
    n = ((uint64_t)degrees * 600000u) + minutes;
    magnitude = latitude ? (((n << 25) + 421875u) / (2u * 421875u))
                         : (((n << 24) + 421875u) / (2u * 421875u));
    raw = (magnitude > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (uint32_t)magnitude;
    if (e7 < 0) {
        // Latitude is sign-magnitude; longitude is a signed two's-complement value
        raw = latitude ? (raw | 0x80000000u) : (uint32_t)(-(int64_t)raw);
    }
    return raw;
}

static int build_lrrp(int32_t lat_e7, int32_t lon_e7, uint16_t speed_knots, uint16_t course, uint8_t *doc)
{
    uint8_t tokens[LRRP_MAX_DOCUMENT];
    uint32_t lat = raw_coordinate(lat_e7, true);
    uint32_t lon = raw_coordinate(lon_e7, false);
    int n = 0;

    tokens[n++] = TOKEN_REQUEST_ID;
    n += put_uintvar(&tokens[n], (uint32_t)sizeof(REPORT_ID));
    memcpy(&tokens[n], REPORT_ID, sizeof(REPORT_ID));
    n += (int)sizeof(REPORT_ID);

    tokens[n++] = TOKEN_POSITION_RADIUS;
    tokens[n++] = (uint8_t)(lat >> 24);
    tokens[n++] = (uint8_t)(lat >> 16);
    tokens[n++] = (uint8_t)(lat >> 8);
    tokens[n++] = (uint8_t)lat;
    tokens[n++] = (uint8_t)(lon >> 24);
    tokens[n++] = (uint8_t)(lon >> 16);
    tokens[n++] = (uint8_t)(lon >> 8);
    tokens[n++] = (uint8_t)lon;
    n += put_uintvar(&tokens[n], 10); // accuracy radius: 10 m
    tokens[n++] = 0;

    if (speed_knots != APRS_DMR_SPEED_UNKNOWN) {
        uint32_t mps128 = (((uint64_t)speed_knots * 65848832u) + 500000u) / 1000000u;

        tokens[n++] = TOKEN_SPEED_HOR;
        n += put_uintvar(&tokens[n], mps128 >> 7);
        tokens[n++] = (uint8_t)(mps128 & 0x7Fu);
    }
    if (course != APRS_DMR_COURSE_UNKNOWN) {
        tokens[n++] = TOKEN_DIRECTION;
        tokens[n++] = (uint8_t)((course % 360u) / 2u);
    }

    {
        uint8_t out[LRRP_MAX_DOCUMENT + 2];
        int len = put_uintvar(out, DOC_TRIGGERED_REPORT);

        len += put_uintvar(&out[len], (uint32_t)n);
        memcpy(&out[len], tokens, (size_t)n);
        memcpy(doc, out, (size_t)(len + n));
        return len + n;
    }
}

static int build_ip(uint32_t destination, uint32_t source, const uint8_t *payload, int length)
{
    uint32_t src_ip = (DMR_CAI_INDIVIDUAL << 24) | source;
    uint32_t dst_ip = (DMR_CAI_INDIVIDUAL << 24) | destination;
    uint16_t id = ip_identification++;
    int udp_length = 8 + length;
    int total = 20 + udp_length;
    uint16_t sum;

    if ((total + 4) > PACKET_MAX) {
        return 0;
    }
    memset(ip_packet, 0, 20);
    ip_packet[0] = 0x45;
    put_u16(&ip_packet[2], (uint16_t)total);
    put_u16(&ip_packet[4], id);
    ip_packet[8] = 64; // group would use TTL 1; this is an individual call
    ip_packet[9] = IP_PROTOCOL_UDP;
    put_u16(&ip_packet[12], (uint16_t)(src_ip >> 16));
    put_u16(&ip_packet[14], (uint16_t)src_ip);
    put_u16(&ip_packet[16], (uint16_t)(dst_ip >> 16));
    put_u16(&ip_packet[18], (uint16_t)dst_ip);
    put_u16(&ip_packet[10], (uint16_t)~ones_complement_sum(0, ip_packet, 20));

    memcpy(&ip_packet[28], payload, (size_t)length);
    put_u16(&ip_packet[20], LRRP_PORT);
    put_u16(&ip_packet[22], LRRP_PORT);
    put_u16(&ip_packet[24], (uint16_t)udp_length);
    put_u16(&ip_packet[26], 0);
    sum = ones_complement_sum(IP_PROTOCOL_UDP + (uint32_t)udp_length, &ip_packet[12], 8);
    sum = (uint16_t)~ones_complement_sum(sum, &ip_packet[20], udp_length);
    put_u16(&ip_packet[26], (sum == 0) ? 0xFFFFu : sum);
    return total;
}

bool aprs_dmr_build_position(int32_t latitude_e7, int32_t longitude_e7, uint16_t speed_knots, uint16_t course,
                             uint32_t destination, uint32_t source, aprs_dmr_tx_t *tx)
{
    uint8_t doc[LRRP_MAX_DOCUMENT];
    uint8_t buf[RATE_12_BYTES];
    uint8_t header[10];
    int doc_length;
    int length;
    int blocks;
    int pad;
    int bursts;
    uint32_t crc;

    if ((tx == NULL) || (destination == 0) || (destination > 0xFFFFFFu) || (source == 0) || (source > 0xFFFFFFu)) {
        return false;
    }
    tx->count = 0;
    doc_length = build_lrrp(latitude_e7, longitude_e7, speed_knots, course, doc);
    if (doc_length <= 0) {
        return false;
    }
    length = build_ip(destination, source, doc, doc_length);
    if (length <= 0) {
        return false;
    }
    blocks = (length + 4 + RATE_12_BYTES - 1) / RATE_12_BYTES;
    pad = (blocks * RATE_12_BYTES) - 4 - length;
    bursts = PREAMBLES + 1 + blocks;
    if ((pad < 0) || (pad > 31) || (blocks > 127) || (bursts > APRS_DMR_MAX_BURSTS) ||
        ((blocks * RATE_12_BYTES) > PACKET_MAX)) {
        return false;
    }

    // Preamble CSBKs: the receivers use them to syncronise before the data header
    for (int i = 0; i < PREAMBLES; i++) {
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x80u | CSBK_OPCODE_PREAMBLE; // Last Block
        buf[1] = 0x00;                         // FID
        buf[2] = 0x80u;                        // Data follows, individual
        buf[3] = (uint8_t)(PREAMBLES - 1 - i + 1 + blocks);
        put_id(&buf[4], destination);
        put_id(&buf[7], source);
        add_burst(tx, APRS_DMR_BURST_CSBK, buf);
    }

    // Unconfirmed data header (ETSI figure 8.3)
    memset(header, 0, sizeof(header));
    header[0] = (uint8_t)(((pad & 0x10u) != 0) ? 0x10u : 0) | DPF_UNCONFIRMED;
    header[1] = (uint8_t)((SAP_IP << 4) | (pad & 0x0Fu));
    put_id(&header[2], destination);
    put_id(&header[5], source);
    header[8] = (uint8_t)(0x80u | blocks); // Full message
    header[9] = 0;                         // FSN: unconfirmed single fragment
    add_burst(tx, APRS_DMR_BURST_DATA_HEADER, header);

    memcpy(packet_data, ip_packet, (size_t)length);
    memset(&packet_data[length], 0, (size_t)pad);
    crc = crc32_dmr(packet_data, length + pad);
    packet_data[length + pad] = (uint8_t)crc;
    packet_data[length + pad + 1] = (uint8_t)(crc >> 8);
    packet_data[length + pad + 2] = (uint8_t)(crc >> 16);
    packet_data[length + pad + 3] = (uint8_t)(crc >> 24);
    for (int i = 0; i < blocks; i++) {
        add_burst(tx, APRS_DMR_BURST_RATE_1_2, &packet_data[i * RATE_12_BYTES]);
    }
    return true;
}
