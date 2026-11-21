"""Experiments on the known-content recordings (dmr_tone.py): thresholds, per-burst level estimates, a matched filter
and Chase decoding of C0 / C1, scored by the frames right after the FEC and the raw bit errors (positions 2 and 3)"""
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dmr_lab as L  # noqa: E402
import dmr_tone as T  # noqa: E402


def bursts(paths, filt):
    out = []
    h = T.rrc(0.2) if filt else None
    for path in paths:
        x = np.fromfile(path, '<i2').astype(float)
        x -= np.mean(x)
        if h is not None:
            x = np.convolve(x, h, mode='same')
        syncs = [s for s in L.find_syncs(x, 0.6) if s[1] == 'MS voice']
        done = set()
        for t0, _, _, g, dc, _ in syncs:
            for k in range(-5, 1):
                t = t0 + k * T.BURST
                sl = round(t / T.BURST)
                if sl in done or t < 4 or t + 133 * L.SPS > len(x) - 4:
                    continue
                done.add(sl)
                out.append((L.interp(x, t + np.arange(132) * L.SPS) - dc) / g)
    return out


def relevel(z, mode, thr):
    """z rescaled so that the slicer's thresholds (0, +-2) fall where they should"""
    if mode == 'fixed':
        # inner / outer threshold thr instead of 2: map piecewise so that thr -> 2
        a = np.abs(z)
        return np.sign(z) * np.where(a < thr, a * 2 / thr, 2 + (a - thr) * 1.0)
    if mode == 'dd':
        # per burst: the four cluster means from the decisions; thresholds at their midpoints, mapped to 0, +-2
        d = np.clip(np.round((z + 3) / 2) * 2 - 3, -3, 3)
        m = {lv: (np.mean(z[d == lv]) if np.any(d == lv) else lv) for lv in (-3, -1, 1, 3)}
        tn, t0, tp = (m[-3] + m[-1]) / 2, (m[-1] + m[1]) / 2, (m[1] + m[3]) / 2
        y = np.empty_like(z)
        for i, v in enumerate(z):
            if v >= tp:
                y[i] = 2 + (v - tp) / max(1e-6, m[3] - tp)
            elif v >= t0:
                y[i] = 2 * (v - t0) / max(1e-6, tp - t0)
            elif v >= tn:
                y[i] = -2 * (t0 - v) / max(1e-6, t0 - tn)
            else:
                y[i] = -2 - (tn - v) / max(1e-6, tn - m[-3])
        return y
    return z


def score(zs, mode, thr, chase):
    right = n = lsb = msb = 0
    for z in zs:
        y = relevel(z, mode, thr)
        bits, rel = T.burst_soft(y)
        for f in (1, 2):
            fb, fr = bits[72 * f:72 * f + 72], rel[72 * f:72 * f + 72]
            right += T.decode_soft(fb, fr, chase) == T.REF
            n += 1
            for ch in range(72):
                e = fb[ch] != T.TONE_BITS[ch]
                if ch % 2:
                    lsb += e
                else:
                    msb += e
    return 100.0 * right / n, 100.0 * msb / (36 * n), 100.0 * lsb / (36 * n), n


if __name__ == '__main__':
    files = sys.argv[1:]
    for filt in (False, True):
        zs = bursts(files, filt)
        for mode, thr in (('none', 2), ('fixed', 1.85), ('fixed', 1.75), ('dd', 0)):
            for chase in (0, 4):
                r, mb, lb, n = score(zs, mode, thr, chase)
                print('filter %-4s levels %-5s %-4s chase %d: right %.1f %%  MSB BER %.2f %%  LSB BER %.2f %%  (%d frames)' % (
                    'rrc' if filt else 'none', mode, thr if mode == 'fixed' else '', chase, r, mb, lb, n), flush=True)
