"""Yaesu System Fusion (YSF / C4FM) layer 2, a model written from Yaesu's "Amateur Radio Digital Standards" rev 1.02
(ysf/facts.md, the spec PDF in ysf/data/spec): FICH, DCH, V/D mode 2 voice (DN), the HC / CC / TC frames, both ways.

  python ysf/py/ysf.py        self test against the test vectors in ysf/facts.md

Bits are lists of 0/1, the first sent first. A frame is 960 bits: sync 40, FICH 200, payload 720.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'vocoder', 'py'))
import ambe_frame  # noqa: E402  (our AMBE+2 72-bit frame <-> 49 bits, DMR's layout)

SYNC = 0xD471C9634D
SYNC_BITS = [(SYNC >> (39 - i)) & 1 for i in range(40)]
FI_HC, FI_CC, FI_TC = 0, 1, 2
DT_VD1, DT_DFR, DT_VD2, DT_VFR = 0, 1, 2, 3


def to_bits(data):
    return [(b >> (7 - i)) & 1 for b in data for i in range(8)]


def to_bytes(bits):
    return bytes(sum(bits[i + j] << (7 - j) for j in range(8)) for i in range(0, len(bits), 8))


# ---- CRC-16 (x^16 + x^12 + x^5 + 1, init 0, inverted: CRC-16/GSM) ----
def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c ^ 0xFFFF


# ---- Golay (24,12): (23,12) with g = 0xC75 and an even parity bit ----
def golay24_encode(d):
    cw23 = ambe_frame.golay23_encode(d)  # d << 11 | parity(11)
    return (cw23 << 1) | (bin(cw23).count('1') & 1)


def golay24_decode(cw):
    """24-bit word -> (12 data bits, errors corrected or None when uncorrectable)."""
    d, e = ambe_frame.golay23_decode(cw >> 1)
    fixed = golay24_encode(d)
    n = bin(fixed ^ cw).count('1')
    return (d, n) if n <= 3 else (d, None)


# ---- convolutional code, rate 1/2, K = 5: G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4 ----
def conv_encode(bits):
    s = 0  # d1 .. d4 in bits 0 .. 3
    out = []
    for d in bits:
        d1, d2, d3, d4 = s & 1, (s >> 1) & 1, (s >> 2) & 1, (s >> 3) & 1
        out += [d ^ d3 ^ d4, d ^ d1 ^ d2 ^ d4]
        s = ((s << 1) | d) & 0xF
    return out


def viterbi(coded, soft=None):
    """Hard (or soft: soft[i] the cost of each coded bit being 1 vs 0, signed) decision Viterbi; ends in state 0.
    Returns (bits, metric)."""
    n = len(coded) // 2
    INF = 1 << 30
    metric = [0] + [INF] * 15
    paths = []
    for k in range(n):
        r1, r2 = coded[2 * k], coded[2 * k + 1]
        new = [INF] * 16
        prev = [0] * 16
        for s in range(16):
            if metric[s] >= INF:
                continue
            d1, d2, d3, d4 = s & 1, (s >> 1) & 1, (s >> 2) & 1, (s >> 3) & 1
            for d in (0, 1):
                g1, g2 = d ^ d3 ^ d4, d ^ d1 ^ d2 ^ d4
                m = metric[s] + (g1 != r1) + (g2 != r2)
                ns = ((s << 1) | d) & 0xF
                if m < new[ns]:
                    new[ns] = m
                    prev[ns] = s
        paths.append(prev)
        metric = new
    s = 0
    out = []
    for k in range(n - 1, -1, -1):
        out.append(s & 1)
        s = paths[k][s]
    return out[::-1], metric[0]


# ---- whitening: PN9 x^9 + x^5 + 1, w0..w8 = 1 0 0 1 0 0 1 1 1, w[n] = w[n-5] ^ w[n-9] ----
def whitening(n):
    w = [1, 0, 0, 1, 0, 0, 1, 1, 1]
    while len(w) < n:
        w.append(w[-5] ^ w[-9])
    return w[:n]


WHITE = whitening(200)


# ---- interleave: S_i (dibit i of the coded bits) at 20 (i mod cols) + i // cols ----
def interleave(coded, cols):
    out = [0] * len(coded)
    for i in range(len(coded) // 2):
        p = 20 * (i % cols) + i // cols
        out[2 * p], out[2 * p + 1] = coded[2 * i], coded[2 * i + 1]
    return out


def deinterleave(air, cols):
    out = [0] * len(air)
    for i in range(len(air) // 2):
        p = 20 * (i % cols) + i // cols
        out[2 * i], out[2 * i + 1] = air[2 * p], air[2 * p + 1]
    return out


# ---- FICH ----
def fich_bytes(fi=FI_CC, cs=2, cm=0, bn=0, bt=0, fn=0, ft=6, dev=0, mr=0, voip=0, dt=DT_VD2, sql=0, sc=0):
    return bytes([(fi << 6) | (cs << 4) | (cm << 2) | bn, (bt << 6) | (fn << 3) | ft,
                  (dev << 6) | (mr << 3) | (voip << 2) | dt, (sql << 7) | sc])


def fich_fields(b):
    return dict(fi=b[0] >> 6, cs=(b[0] >> 4) & 3, cm=(b[0] >> 2) & 3, bn=b[0] & 3, bt=b[1] >> 6, fn=(b[1] >> 3) & 7,
                ft=b[1] & 7, dev=(b[2] >> 6) & 1, mr=(b[2] >> 3) & 7, voip=(b[2] >> 2) & 1, dt=b[2] & 3,
                sql=b[3] >> 7, sc=b[3] & 0x7F)


def fich_encode(info4):
    c = crc16(info4)
    data = info4 + bytes([c >> 8, c & 0xFF])
    bits = []
    for k in range(4):
        d = int.from_bytes(data, 'big') >> (36 - 12 * k) & 0xFFF
        cw = golay24_encode(d)
        bits += [(cw >> (23 - i)) & 1 for i in range(24)]
    return interleave(conv_encode(bits + [0, 0, 0, 0]), 5)


def fich_decode(air200):
    """-> (4 info bytes or None, viterbi metric, golay corrections)"""
    bits, metric = viterbi(deinterleave(air200, 5))
    v, fixes = 0, 0
    for k in range(4):
        cw = 0
        for b in bits[24 * k:24 * k + 24]:
            cw = (cw << 1) | b
        d, e = golay24_decode(cw)
        if e is None:
            return None, metric, None
        fixes += e
        v = (v << 12) | d
    data = v.to_bytes(6, 'big')
    if crc16(data[:4]) != (data[4] << 8 | data[5]):
        return None, metric, fixes
    return data[:4], metric, fixes


# ---- DCH: whitened data, the CRC over the whitened bytes, tail, conv, interleave ----
def dch_encode(data):
    """10 bytes (V/D mode 2) -> 200 bits; 20 bytes (HC / TC / V/D mode 1) -> 360 bits."""
    n = len(data)
    w = to_bytes(WHITE[:8 * n])
    wd = bytes(a ^ b for a, b in zip(data, w))
    c = crc16(wd)
    bits = to_bits(wd + bytes([c >> 8, c & 0xFF])) + [0, 0, 0, 0]
    return interleave(conv_encode(bits), 5 if n == 10 else 9)


def dch_decode(air):
    """200 or 360 bits -> (data bytes or None when the CRC fails, viterbi metric)"""
    n = 10 if len(air) == 200 else 20
    bits, metric = viterbi(deinterleave(air, 5 if n == 10 else 9))
    wd = to_bytes(bits[:8 * n + 16])
    if crc16(wd[:n]) != (wd[n] << 8 | wd[n + 1]):
        return None, metric
    w = to_bytes(WHITE[:8 * n])
    return bytes(a ^ b for a, b in zip(wd[:n], w)), metric


# ---- V/D mode 2 voice: 49 bits -> 104 ----
def vd2_voice_encode(d49):
    v = []
    for b in d49[:27]:
        v += [b, b, b]
    v += list(d49[27:]) + [0]
    v = [a ^ b for a, b in zip(v, WHITE)]
    out = [0] * 104
    for i in range(104):
        out[4 * (i % 26) + i // 26] = v[i]
    return out


def vd2_voice_decode(air104):
    """-> (49 bits, triplets not unanimous)"""
    v = [air104[4 * (i % 26) + i // 26] ^ WHITE[i] for i in range(104)]
    d, errs = [], 0
    for k in range(27):
        t = v[3 * k:3 * k + 3]
        s = sum(t)
        errs += s in (1, 2)
        d.append(1 if s >= 2 else 0)
    return d + v[81:103], errs


def d49_to_ambe72(d49):
    return ambe_frame.encode_frame(list(d49))


def ambe72_to_d49(bits72):
    return ambe_frame.decode_frame(bits72)[0]


# ---- frames ----
def pad10(s):
    return (s.encode('ascii') + b' ' * 10)[:10]


def frame_hc_tc(fi, csd1, csd2, ft=6, dt=DT_VD2, mr=0, cm=0, sc=0):
    """a header (FI_HC) or terminator (FI_TC): CSD1 / CSD2 (20 bytes each) in DCH-1 / DCH-2 alternating"""
    fich = fich_encode(fich_bytes(fi=fi, cm=cm, fn=0, ft=ft, dt=dt, mr=mr, sc=sc))
    d1, d2 = dch_encode(csd1), dch_encode(csd2)
    pay = []
    for j in range(5):
        pay += d1[72 * j:72 * j + 72] + d2[72 * j:72 * j + 72]
    return SYNC_BITS + fich + pay


def frame_vd2(fn, ft, dch10, voices49, mr=0, cm=0, sc=0):
    """a V/D mode 2 CC: its DCH (10 bytes, by FN) and 5 voice frames of 49 bits"""
    fich = fich_encode(fich_bytes(fi=FI_CC, cm=cm, fn=fn, ft=ft, dt=DT_VD2, mr=mr, sc=sc))
    d = dch_encode(dch10)
    pay = []
    for j in range(5):
        pay += d[40 * j:40 * j + 40] + vd2_voice_encode(voices49[j])
    return SYNC_BITS + fich + pay


def vd2_dch_content(fn, dst, src, radio_id='     '):
    """FT 6: FN 0 dest, 1 src, 2 down, 3 up, 4 Rem1+2, 5 Rem3+4, 6 DT1"""
    if fn == 0:
        return pad10(dst)
    if fn == 1:
        return pad10(src)
    if fn == 5:
        return b' ' * 5 + (radio_id.encode('ascii') + b' ' * 5)[:5]
    if fn == 6:
        return bytes(10)
    return b' ' * 10


def parse_frame(bits):
    """960 bits (sync first) -> dict: fich fields, the decoded DCH(s), voice (list of (49 bits, errors))"""
    out = {'fich': None}
    info, metric, fixes = fich_decode(bits[40:240])
    out['fich_metric'] = metric
    if info is None:
        return out
    f = fich_fields(info)
    out['fich'] = f
    pay = bits[240:]
    if f['fi'] in (FI_HC, FI_TC) or f['dt'] == DT_DFR:
        d1, d2 = [], []
        for j in range(5):
            d1 += pay[144 * j:144 * j + 72]
            d2 += pay[144 * j + 72:144 * j + 144]
        out['dch'] = [dch_decode(d1)[0], dch_decode(d2)[0]]
    elif f['dt'] == DT_VD2:
        d, voice = [], []
        for j in range(5):
            d += pay[144 * j:144 * j + 40]
            voice.append(vd2_voice_decode(pay[144 * j + 40:144 * j + 144]))
        out['dch'] = [dch_decode(d)[0]]
        out['voice'] = voice
    elif f['dt'] == DT_VD1:
        d, voice = [], []
        for j in range(5):
            d += pay[144 * j:144 * j + 72]
            v72 = pay[144 * j + 72:144 * j + 144]
            dd, e0, e1 = ambe_frame.decode_frame(v72)
            voice.append((dd, e0 + e1))
        out['dch'] = [dch_decode(d)[0]]
        out['voice'] = voice
    return out


def self_test():
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok &= good
        print('%-34s %s' % (name, 'ok' if good else 'FAIL got %s want %s' % (got, want)))

    check('crc16 "123456789"', '%04X' % crc16(b'123456789'), 'CE3C')
    check('golay24 0x001', '%06X' % golay24_encode(0x001), '0018EB')
    check('golay24 0x800', '%06X' % golay24_encode(0x800), '800C75')
    check('whitening bytes', to_bytes(WHITE[:160]).hex().upper(), '93D751219C2F6CD0EF0FF83DF1732094ED1E7CD8')
    vectors = [(dict(fi=FI_HC, ft=6), '2006020', '213D38A4BE3121FC3123CF8280E0944BB300767C4FDC230728'),
               (dict(fi=FI_CC, fn=0, ft=6), '6006020', '11CD384DC1F2B1FC46DD8E7283E40E077302E857C51C23F260'),
               (dict(fi=FI_CC, fn=1, ft=6), '600E020', '11DD40DC62F2A2F333558E501274F00743F08CA1C5DF80A637'),
               (dict(fi=FI_TC, ft=6), 'A006020', 'E29D38784170C1FF01B883B28296BDC1F300F7378C1C23AAD8'),
               (dict(fi=FI_HC, ft=7, mr=2), '2007120', '21365C9A1A3121A0D7DDCFB5909C5C4BA60373D14FC5119AF7')]
    for kw, _, air in vectors:
        info = fich_bytes(**kw)
        enc = to_bytes(fich_encode(info)).hex().upper()
        check('FICH %s' % info.hex().upper(), enc, air)
        dec = fich_decode(to_bits(bytes.fromhex(air)))[0]
        check('  decoded', dec, info)
    for text, air in (('**********', 'E77BFC86E1485358796DA9F0EBFDCE18FB301A5734DF1946E0'),
                      ('PY2DZA    ', 'F2C8FB6E75B4798606C322306574D6F9B207E0DA6C2821D90F')):
        check('DCH %r' % text, to_bytes(dch_encode(text.encode())).hex().upper(), air)
        check('  decoded', dch_decode(to_bits(bytes.fromhex(air)))[0], text.encode())
    hc = frame_hc_tc(FI_HC, b'**********PY2DZA    ', b' ' * 20)
    want = ('D471C9634D213D38A4BE3121FC3123CF8280E0944BB300767C4FDC230728F3F8FB1C1853118134DF'
            '2363C017607F1C88BEA5E04FC29426FF9BB9A85ABA46921014BE11A92CD37A891B9C7575A8F77FB8'
            '1110F2C664F3083EE8A7C9948F3DEF0E84251E49946425B6063C570E3151BC1373F60DFBD5890193')
    check('HC frame', to_bytes(hc).hex().upper(), want)
    sil = to_bits(bytes.fromhex('7BB28E4336E4A2397849336833'))
    d49, errs = vd2_voice_decode(sil)
    want49 = [int(c) for c in '111110000000' '000110101001' '1001111110001100111000001']
    check('VD2 silence -> 49 bits', (d49, errs), (want49, 0))
    check('VD2 silence <- 49 bits', to_bytes(vd2_voice_encode(want49)).hex().upper(), '7BB28E4336E4A2397849336833')
    dmr_sil = to_bits(bytes.fromhex('B9E881526173002A6B'))
    check('DMR silence frame -> same 49 bits', ambe72_to_d49(dmr_sil), want49)
    check('49 bits -> DMR silence frame', to_bytes(d49_to_ambe72(want49)).hex().upper(), 'B9E881526173002A6B')
    print('all ok' if ok else 'FAILURES')
    return ok


if __name__ == '__main__':
    sys.exit(0 if self_test() else 1)
