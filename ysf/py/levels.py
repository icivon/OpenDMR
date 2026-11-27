"""The received symbol levels of a recording (ysf/data/cap/*.raw, 8 kHz): at each sync (found by the least squares fit),
the frame's 480 symbols scaled by the sync's gain and DC; their histogram, the four levels' means and spreads, the
gain (the deviation as received, in recording units per symbol level), the DC (frequency offset). Also the
samples' spectrum peak, to see the signal's bandwidth.

  python ysf/py/levels.py ysf/data/cap/own1.raw [more.raw ...]
"""
import sys

import numpy as np

import demod


def analyse(path):
    x = demod.read(path)
    x = x - np.mean(x)
    y = np.convolve(x, demod.rrc(), mode='same')
    syncs = []
    t = 2.0
    while t < len(y) - 480 * demod.SPS - 10:
        g, dc, err = demod.fit(y, t, 1)
        if g > 0 and err < 0.6:
            best = (err, t, g, dc)
            for s in np.arange(-1.0, 1.0001, 1 / 12):
                g2, dc2, e2 = demod.fit(y, t + s, 1)
                if g2 > 0 and e2 < best[0]:
                    best = (e2, t + s, g2, dc2)
            syncs.append(best)
            t = best[1] + 470 * demod.SPS
        else:
            t += 1 / 3
    allv = []
    for err, t0, g, dc in syncs:
        allv.append(demod.symbols(y, t0, g, dc, 1))
    print('%s: %d syncs (fit err %s), gain %s, dc %s' % (
        path, len(syncs), ['%.2f' % s[0] for s in syncs], ['%.0f' % s[2] for s in syncs], ['%.0f' % s[3] for s in syncs]))
    if not allv:
        return
    v = np.concatenate(allv)
    h, e = np.histogram(v, bins=np.arange(-5, 5.01, 0.25))
    print('  histogram (levels -5 .. 5 in 0.25):')
    print('  ' + ' '.join('%d' % c for c in h))
    for lv in (-3, -1, 1, 3):
        sel = v[np.abs(v - lv) < 1]
        print('  level %+d: n %4d mean %+.2f sd %.2f' % (lv, len(sel), sel.mean() if len(sel) else 0,
                                                         sel.std() if len(sel) else 0))
    print('  raw peak %.0f, rms %.0f' % (np.percentile(np.abs(x), 99.5), np.sqrt(np.mean(x ** 2))))


for p in sys.argv[1:]:
    analyse(p)
