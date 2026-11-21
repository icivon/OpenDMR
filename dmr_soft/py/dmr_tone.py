"""DMR receive against known content (the transmitter's vocoder_tx_test: every AMBE+2 frame the 1031 Hz tone frame):
the chip's frames (RX_CAPTURE .bit) or our software sync / demodulation of discriminator recordings, scored the same:
frames right after the FEC (all 49 parameter bits) and the raw bit error rate before it, by position in the burst
(the first frame of our own transmitter's bursts is damaged on the air).

  python dmr_soft/py/dmr_tone.py --bit chip.bit
  python dmr_soft/py/dmr_tone.py rec0.raw rec1.raw ... [--chase N] [--filter rrc|none]
"""
import argparse
import itertools
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'vocoder' / 'py'))
import dmr_lab as L  # noqa: E402
import ambe_frame as A  # noqa: E402

TONE = bytes.fromhex('CEA8FE83ACC458200A')
TONE_BITS = [(TONE[j // 8] >> (7 - j % 8)) & 1 for j in range(72)]
REF = A.decode_frame(TONE_BITS)[0]
BURST = 2 * 144 * L.SPS


def frame_bits(b):
    return [(b[j // 8] >> (7 - j % 8)) & 1 for j in range(72)]


# where each of the 72 channel bits goes: (vector, index), as ambe_frame.deinterleave
POS = []
for i in range(36):
    POS.append((A.W[i], A.X[i]))
    POS.append((A.Y[i], A.Z[i]))


def golay23_chase(hard, rel, p):
    """hard: 23-bit block (MSB first as an int over bits 22..0), rel: reliability of each bit (index 22..0); the
    algebraic decoder on the received word and on it with every combination of its p least reliable bits flipped:
    the result whose codeword differs least (in reliability) from what was received"""
    order = sorted(range(23), key=lambda j: rel[j])[:p]
    best = None
    for k in range(p + 1):
        for flips in itertools.combinations(order, k):
            w = hard
            for j in flips:
                w ^= 1 << j
            data, _ = A.golay23_decode(w)
            code = A.golay23_encode(data)
            diff = code ^ hard
            cost = sum(rel[j] for j in range(23) if (diff >> j) & 1)
            if best is None or cost < best[0]:
                best = (cost, data)
    return best[1]


def decode_soft(bits, rel, chase):
    """72 hard bits and their reliabilities -> 49 parameter bits (Chase on C0 and C1 when chase > 0)"""
    if chase <= 0:
        return A.decode_frame(bits)[0]
    fr = A.deinterleave(bits)
    fr_rel = [[0.0] * 24, [0.0] * 23, [0.0] * 11, [0.0] * 14]
    for ch, (v, x) in enumerate(POS):
        fr_rel[v][x] = rel[ch]
    c0w = A._vec_to_int(fr[0], 23, 1)
    rel0 = {22 - (23 - j): 0 for j in range(1)}  # unused
    r0 = [fr_rel[0][j + 1] for j in range(23)]   # vector bit j+1 -> int bit j
    c0 = golay23_chase(c0w, r0, chase)
    pn = A._pn(c0)
    for k, j in enumerate(range(22, -1, -1)):
        fr[1][j] ^= pn[k]
    c1w = A._vec_to_int(fr[1], 22, 0)
    r1 = [fr_rel[1][j] for j in range(23)]
    c1 = golay23_chase(c1w, r1, chase)
    d = [(c0 >> (11 - i)) & 1 for i in range(12)]
    d += [(c1 >> (11 - i)) & 1 for i in range(12)]
    d += [fr[2][j] for j in range(10, -1, -1)]
    d += [fr[3][j] for j in range(13, -1, -1)]
    return d


def burst_soft(z):
    """132 soft symbols -> the 216 voice bits (hard) and their reliabilities: the high bit by |z|, the low one by
    ||z| - 2|"""
    bits, rel = [], []
    for v in z:
        bits += [1 if v < 0 else 0, 1 if abs(v) > 2 else 0]
        rel += [abs(v), abs(abs(v) - 2)]
    keep = list(range(108)) + list(range(156, 264))
    return [bits[i] for i in keep], [rel[i] for i in keep]


def report(results, label):
    """results: (position in the burst, right after FEC, raw bit errors)"""
    n = len(results)
    out = '%s: %d frames, right after FEC %.1f %%, raw BER %.2f %%' % (
        label, n, 100.0 * sum(r[1] for r in results) / max(1, n), 100.0 * sum(r[2] for r in results) / max(1, 72 * n))
    for p in range(3):
        q = [r for r in results if r[0] == p]
        if q:
            out += '\n   position %d: right %.1f %%, raw BER %.2f %%' % (
                p + 1, 100.0 * sum(r[1] for r in q) / len(q), 100.0 * sum(r[2] for r in q) / (72 * len(q)))
    print(out)


def from_bit(path):
    d = Path(path).read_bytes()
    frames = [d[9 * i:9 * i + 9] for i in range(len(d) // 9)]
    # the alignment: the position where the first frame is worst (the damaged one) is position 0
    best = None
    for off in range(3):
        raw = [sum(a != b for a, b in zip(frame_bits(frames[i]), TONE_BITS)) for i in range(off, len(frames), 3)]
        if best is None or np.mean(raw) > best[0]:
            best = (np.mean(raw), off)
    off = best[1]
    res = []
    for i, f in enumerate(frames):
        b = frame_bits(f)
        res.append(((i - off) % 3, A.decode_frame(b)[0] == REF, sum(x != y for x, y in zip(b, TONE_BITS))))
    report(res, 'chip ' + path)


def from_recordings(paths, chase, filt, threshold):
    res = []
    h = None
    if filt == 'rrc':
        h = rrc(0.2)
    for path in paths:
        x = np.fromfile(path, '<i2').astype(float)
        x -= np.mean(x)
        if h is not None:
            x = np.convolve(x, h, mode='same')
        syncs = [s for s in L.find_syncs(x, threshold) if s[1] == 'MS voice']
        done = set()
        for t0, name, pol, g, dc, e in syncs:
            for k in range(-5, 1):
                t = t0 + k * BURST
                slot = round(t / BURST)
                if slot in done or t < 4 or t + 133 * L.SPS > len(x) - 4:
                    continue
                done.add(slot)
                z = (L.interp(x, t + np.arange(132) * L.SPS) - dc) / g
                if np.mean((z - np.clip(np.round((z + 3) / 2) * 2 - 3, -3, 3)) ** 2) > 0.6:
                    continue
                bits, rel = burst_soft(z)
                for f in range(3):
                    fb, fr = bits[72 * f:72 * f + 72], rel[72 * f:72 * f + 72]
                    res.append((f, decode_soft(fb, fr, chase) == REF, sum(a != b for a, b in zip(fb, TONE_BITS))))
    report(res, 'software (chase %d, filter %s)' % (chase, filt))


def rrc(alpha, fs=8000, rs=4800, span=6):
    n = np.arange(-int(span * fs / rs), int(span * fs / rs) + 1)
    t = n / fs * rs
    h = np.zeros(len(t))
    for i, x in enumerate(t):
        if abs(x) < 1e-9:
            h[i] = 1 - alpha + 4 * alpha / np.pi
        elif abs(abs(x) - 1 / (4 * alpha)) < 1e-9:
            h[i] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
        else:
            h[i] = (np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha))) / (np.pi * x * (1 - (4 * alpha * x) ** 2))
    return h / h.sum()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--bit', action='store_true')
    ap.add_argument('--chase', type=int, default=0)
    ap.add_argument('--filter', default='none')
    ap.add_argument('--threshold', type=float, default=0.6)
    a = ap.parse_args()
    if a.bit:
        for f in a.files:
            from_bit(f)
    else:
        from_recordings(a.files, a.chase, a.filter, a.threshold)


if __name__ == '__main__':
    main()
