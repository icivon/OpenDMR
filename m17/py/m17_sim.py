"""An M17 transmission as the radio's 8 kHz discriminator recording would give it (for m17/c/m17_demod_test.c): the
frame file's symbols (a 40 ms preamble first, the standard one) through the RRC filter (alpha 0.5) at 48 kHz,
resampled to 8 kHz with a clock offset, a gain like the radio's (an outer symbol ~ +-7500), DC, noise; noise before
and after.

  python m17/py/m17_sim.py frames.txt out.raw [--snr 20] [--dc 500] [--ppm 50] [--seconds 3]
"""
import argparse
import sys

import numpy as np
from scipy.signal import resample_poly

sys.path.insert(0, __import__('os').path.dirname(__file__))
from af_fit import SYM, rrc  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames')
    ap.add_argument('out')
    ap.add_argument('--snr', type=float, default=20.0)
    ap.add_argument('--dc', type=float, default=500.0)
    ap.add_argument('--ppm', type=float, default=50.0)
    ap.add_argument('--seconds', type=float, default=3.0)
    ap.add_argument('--offset', type=float, default=0.37, help='fraction of a sample the start is shifted')
    a = ap.parse_args()
    syms = [3, -3] * 96
    t_max = a.seconds
    for line in open(a.frames):
        kind, n, h = line.split()
        for b in bytes.fromhex(h):
            syms += [SYM[(b >> s) & 3] for s in (6, 4, 2, 0)]
        if len(syms) / 4800 > t_max:
            break
    s = np.array(syms, float)
    up = np.zeros(len(s) * 10)
    up[::10] = s
    y = np.convolve(up, rrc(0.5, 10, 8) * 10, 'same')
    # 48 kHz -> 8 kHz with a clock offset and a fractional start
    t = (np.arange(int(len(y) / 6) - 2) * 6 * (1 + a.ppm * 1e-6)) + a.offset * 6
    x = np.interp(t, np.arange(len(y)), y)
    x = x / 3 * 7500
    rng = np.random.default_rng(17)
    lead = rng.normal(0, 9000, 4000)
    x = np.concatenate([lead, x + a.dc + rng.normal(0, 7500 / 3 * 10 ** (-a.snr / 20), len(x)), rng.normal(0, 9000, 2000)])
    np.clip(x, -32767, 32767).astype('<i2').tofile(a.out)
    print('%s: %.2f s, %d symbols' % (a.out, len(x) / 8000, len(s)))


if __name__ == '__main__':
    main()
