"""DMR from the discriminator recording (8 kHz, the M17 path: FM_BANDWIDTH 0x0F), demodulated in software, offline:
is it as good as the HR_C7000's own DMR receive? Against the bursts of the hotspot's test file (pistar-signal-gen
1031Hz_test_dmr.txt, BS sourced, TS1; the modem's idle bursts on TS2).

Per burst: its 48-bit sync (24 symbols, all +-3) found by a least-squares fit of gain and DC at fractional symbol
times (cubic interpolation, 5/3 samples a symbol), as m17_demod.c does; then the 132 symbols of the burst sampled with
that timing, gain and DC and sliced (0, +-2 gain). The voice bursts are compared with the file's symbol by symbol.

  python dmr_soft/py/dmr_lab.py dmr_soft/data/dmr_ts1_1.raw [...]
"""
import sys

import numpy as np

SPS = 8000 / 4800
SYNCS = {0x755FD7DF75F7: 'BS voice', 0xDFF57D75DF5D: 'BS data', 0x7F7D5DD57DFD: 'MS voice', 0xD5D7F77FD757: 'MS data',
         0x5D577F7757FF: 'DMO1 voice', 0xF7FDD5DDFD55: 'DMO1 data', 0x7DFFD5F55D5F: 'DMO2 voice',
         0xD7557F5FF7F5: 'DMO2 data'}
LEVEL = {1: 3, 0: 1, 2: -1, 3: -3}  # dibit -> symbol
FILE = 'E:/exam/pistar-signal-gen/files/1031Hz_test_dmr.txt'


def symbols_of(value, n):
    return np.array([LEVEL[(value >> (2 * (n - 1 - i))) & 3] for i in range(n)], float)


def burst_symbols(data):
    v = int.from_bytes(data, 'big')
    return symbols_of(v, 132)


def interp(x, t):
    """cubic (Lagrange, 4 points) at fractional sample times t"""
    i = np.floor(t).astype(int)
    f = t - i
    i = np.clip(i, 1, len(x) - 3)
    xm1, x0, x1, x2 = x[i - 1], x[i], x[i + 1], x[i + 2]
    return (-f * (f - 1) * (f - 2) / 6 * xm1 + (f + 1) * (f - 1) * (f - 2) / 2 * x0
            - (f + 1) * f * (f - 2) / 2 * x1 + (f + 1) * f * (f - 1) / 6 * x2)


def fit(y, s):
    """y = g s + dc, least squares: g, dc, the normalised error (level^2)"""
    A = np.vstack([s, np.ones_like(s)]).T
    (g, dc), *_ = np.linalg.lstsq(A, y, rcond=None)
    if g == 0:
        return 0, 0, 1e9
    e = np.mean(((y - dc) / g - s) ** 2)
    return g, dc, e


def find_syncs(x, threshold=0.25):
    """every burst sync: (time of the burst's first symbol, kind, polarity, gain, dc, error)"""
    # polarity +1 only: a BS voice sync inverted is the BS data sync (and the same for the others)
    pats = [(v, name, pol, symbols_of(v, 24) * pol) for v, name in SYNCS.items() for pol in (1,)]
    found = []
    t = 60.0
    step = 1 / 3
    last = -1e9
    while t + (132 + 4) * SPS < len(x):
        # the sync's 24 symbols start at symbol 54 of the burst
        ts = t + (54 + np.arange(24)) * SPS
        y = interp(x, ts)
        best = None
        for v, name, pol, s in pats:
            if np.sum(np.sign(y - np.mean(y)) == np.sign(s)) < 20:
                continue
            g, dc, e = fit(y, s)
            if g > 0 and e < threshold and (best is None or e < best[5]):
                best = (t, name, pol, g, dc, e)
        if best and best[0] - last > 100 * SPS:
            # refine to 1/12 sample around it
            tb = best
            for dt in np.arange(-0.5, 0.5, 1 / 12):
                ts = best[0] + dt + (54 + np.arange(24)) * SPS
                y = interp(x, ts)
                s = symbols_of([k for k, n in SYNCS.items() if n == best[1]][0], 24) * best[2]
                g, dc, e = fit(y, s)
                if g > 0 and e < tb[5]:
                    tb = (best[0] + dt, best[1], best[2], g, dc, e)
            found.append(tb)
            last = tb[0]
            t = tb[0] + 120 * SPS
        else:
            t += step
    return found


def main():
    lines = [l.split() for l in open(FILE)]
    file_bursts = [burst_symbols(bytes.fromhex(l[2])) for l in lines if l[0].startswith('DMR')]
    names = ['header', 'A', 'B', 'C', 'D', 'E', 'F', 'terminator']
    tot = {'sym': 0, 'err': 0, 'bursts': 0, 'idle': 0}
    for path in sys.argv[1:]:
        x = np.fromfile(path, '<i2').astype(float)
        x -= np.mean(x)
        syncs = find_syncs(x)
        if not syncs:
            print(path, 'no sync')
            continue
        slot = 144 * SPS
        # every slot from the first sync on, timing / gain / DC from the nearest sync (re-anchored at each)
        t0 = syncs[0][0]
        k0 = -int(t0 // slot)
        counts = {}
        line = []
        for k in range(k0, int((len(x) - t0) / slot)):
            t = t0 + k * slot
            near = min(syncs, key=lambda s: abs(s[0] - t))
            n = round((t - near[0]) / slot)
            t = near[0] + n * slot
            if t < 4 or t + 133 * SPS > len(x) - 4:
                continue
            g, dc = near[3], near[4]
            z = (interp(x, t + np.arange(132) * SPS) - dc) / g
            dec = np.where(z > 2, 3, np.where(z > 0, 1, np.where(z > -2, -1, -3)))
            errs = [int(np.sum(dec != ref)) for ref in file_bursts]
            i = int(np.argmin(errs))
            if errs[i] < 30:
                tot['sym'] += 132
                tot['err'] += errs[i]
                tot['bursts'] += 1
                line.append('%s:%d' % (names[i], errs[i]))
            else:
                tot['idle'] += 1
                line.append('-')
        print('%s: %d syncs; slots: %s' % (path, len(syncs), ' '.join(line)))
    print('TS1 bursts %d (TS2 idle %d): symbol errors %d of %d (%.4f %%)' % (
        tot['bursts'], tot['idle'], tot['err'], tot['sym'], 100.0 * tot['err'] / max(1, tot['sym'])))


if __name__ == '__main__':
    main()
