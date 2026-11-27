"""YSF from an FM discriminator recording: resampled to 8 kHz (as the radio records), the sync found by correlation,
each frame's 480 symbols read at 5/3 samples a symbol (gain and DC fitted to its sync), decoded with ysf.py.

  python ysf/py/demod.py rec.wav|rec.dis|rec.raw [--invert] [--bit out.bit] [--quiet]

.wav any rate (mono), .dis S16LE 48 kHz, .raw S16LE 8 kHz (the radio's recording). Writes the voice as AMBE frames
(9 bytes, DMR's 72-bit layout) to --bit for vocoder/py/decode.py.
"""
import argparse
import wave

import numpy as np
from scipy.signal import resample_poly

import ysf

FS = 8000
SPS = FS / 4800.0
LEVELS = {1: 3, 0: 1, 2: -1, 3: -3}  # dibit -> level


def read(path):
    if path.endswith('.dis'):
        return resample_poly(np.fromfile(path, '<i2').astype(float), 1, 6)
    if path.endswith('.raw'):
        return np.fromfile(path, '<i2').astype(float)
    w = wave.open(path)
    n, sw, rate = w.getnframes(), w.getsampwidth(), w.getframerate()
    raw = np.frombuffer(w.readframes(n), np.uint8)
    if sw == 3:
        b = raw.reshape(-1, 3).astype(np.int32)
        x = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
        x = np.where(x & 0x800000, x - (1 << 24), x).astype(float)
    else:
        x = np.frombuffer(raw.tobytes(), '<i2').astype(float)
    if w.getnchannels() > 1:
        x = x[::w.getnchannels()]
    from math import gcd
    g = gcd(FS, rate)
    return resample_poly(x, FS // g, rate // g)


def rrc(alpha=0.2, span=6, sps=SPS):
    t = np.arange(-span * sps, span * sps + 1e-9) / sps
    t = np.arange(int(-span * sps), int(span * sps) + 1) / sps
    h = np.zeros_like(t)
    for i, x in enumerate(t):
        if abs(x) < 1e-9:
            h[i] = 1 - alpha + 4 * alpha / np.pi
        elif abs(abs(x) - 1 / (4 * alpha)) < 1e-9:
            h[i] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) +
                                         (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
        else:
            h[i] = (np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha))) / \
                   (np.pi * x * (1 - (4 * alpha * x) ** 2))
    return h / h.sum()


SYNC_LV = np.array([LEVELS[(ysf.SYNC >> (38 - 2 * k)) & 3] for k in range(20)], float)


def at(x, t):
    i = np.floor(t).astype(int)
    f = t - i
    i = np.clip(i, 1, len(x) - 3)
    p0, p1, p2, p3 = x[i - 1], x[i], x[i + 1], x[i + 2]
    return p1 + 0.5 * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)))


def fit(x, t0, sign):
    v = at(x, t0 + np.arange(20) * SPS)
    A = np.vstack([sign * SYNC_LV, np.ones(20)]).T
    (g, dc), res, *_ = np.linalg.lstsq(A, v, rcond=None)
    err = np.mean((v - (g * sign * SYNC_LV + dc)) ** 2) / max(g * g, 1e-9)
    return g, dc, err


def find_syncs(x, sign):
    """sync starts (in samples, fractional) where the fit is good"""
    out = []
    t = 2.0
    end = len(x) - 480 * SPS - 10
    while t < end:
        g, dc, err = fit(x, t, sign)
        if g > 0 and err < 0.35:
            best = (err, t, g, dc)
            for s in np.arange(-1.0, 1.0001, 1 / 12):
                g2, dc2, e2 = fit(x, t + s, sign)
                if g2 > 0 and e2 < best[0]:
                    best = (e2, t + s, g2, dc2)
            out.append(best)
            t = best[1] + 470 * SPS
        else:
            t += 1 / 3
    return out


def symbols(x, t0, g, dc, sign, n=480):
    v = (at(x, t0 + np.arange(n) * SPS) - dc) / g * sign
    return v


def decide(v):
    return [1 if s >= 2 else 0 if s >= 0 else 2 if s >= -2 else 3 for s in v]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--invert', action='store_true')
    ap.add_argument('--bit')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--no-rrc', action='store_true')
    a = ap.parse_args()
    x = read(a.path)
    x = x - np.mean(x)
    if not a.no_rrc:
        x = np.convolve(x, rrc(), mode='same')
    sign = -1 if a.invert else 1
    syncs = find_syncs(x, sign)
    print('%s: %.1f s, %d syncs' % (a.path, len(x) / FS, len(syncs)))
    frames72 = []
    stats = dict(fich=0, fich_bad=0, dch_ok=0, dch_bad=0, voice=0, rep_err=0, rep_bits=0)
    for err, t, g, dc in syncs:
        v = symbols(x, t, g, dc, sign)
        d = decide(v)
        bits = []
        for s in d:
            bits += [s >> 1, s & 1]
        sync_wrong = sum(b != c for b, c in zip(bits[:40], ysf.SYNC_BITS))
        p = ysf.parse_frame(bits)
        f = p['fich']
        if f is None:
            stats['fich_bad'] += 1
            if not a.quiet:
                print('%7.3f s  sync %d wrong  FICH bad (viterbi %d)' % (t / FS, sync_wrong, p['fich_metric']))
            continue
        stats['fich'] += 1
        dch = p.get('dch', [])
        for dd in dch:
            stats['dch_ok' if dd is not None else 'dch_bad'] += 1
        desc = ' '.join(repr(dd.decode('latin1')) if dd is not None else 'CRC!' for dd in dch)
        vtxt = ''
        if 'voice' in p:
            errs = [e for _, e in p['voice']]
            stats['voice'] += len(errs)
            stats['rep_err'] += sum(errs)
            stats['rep_bits'] += 27 * len(errs)
            vtxt = ' voice errs %s' % errs
            for d49, _ in p['voice']:
                frames72.append(ysf.to_bytes(ysf.d49_to_ambe72(d49)))
        if not a.quiet:
            print('%7.3f s  sync %d  FI %d FN %d FT %d DT %d MR %d CM %d SC %d (vit %d) %s%s' % (
                t / FS, sync_wrong, f['fi'], f['fn'], f['ft'], f['dt'], f['mr'], f['cm'], f['sc'], p['fich_metric'],
                desc, vtxt))
    print('FICH ok %d bad %d, DCH ok %d bad %d, voice frames %d, repetition BER %.2f %%' % (
        stats['fich'], stats['fich_bad'], stats['dch_ok'], stats['dch_bad'], stats['voice'],
        100.0 * stats['rep_err'] / max(stats['rep_bits'], 1)))
    if a.bit:
        open(a.bit, 'wb').write(b''.join(frames72))
        print('wrote %d AMBE frames to %s' % (len(frames72), a.bit))


if __name__ == '__main__':
    main()
