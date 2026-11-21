"""Fit a discriminator recording (af_capture.py, 8 kHz) to the M17 transmission it came from: the symbols of the frame
file through the RRC (alpha 0.5) filter, aligned by correlation; the gain (units per symbol level) and the offset
(units: the carrier's frequency error) by least squares.

  python m17/py/af_fit.py capture.af.npy frames.txt
"""
import sys

import numpy as np

SYM = {1: 3, 0: 1, 2: -1, 3: -3}


def symbols(path):
    out = []
    for line in open(path):
        kind, n, h = line.split()
        if kind == 'M17EOT':
            continue
        for b in bytes.fromhex(h):
            out += [SYM[(b >> s) & 3] for s in (6, 4, 2, 0)]
    return np.array(out, float)


def rrc(alpha, sps, span):
    t = np.arange(-span * sps, span * sps + 1) / sps
    h = np.zeros_like(t)
    for i, x in enumerate(t):
        if abs(x) < 1e-9:
            h[i] = 1 - alpha + 4 * alpha / np.pi
        elif abs(abs(x) - 1 / (4 * alpha)) < 1e-9:
            h[i] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
        else:
            h[i] = (np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha))) / (np.pi * x * (1 - (4 * alpha * x) ** 2))
    return h / h.sum()


def main():
    x = np.load(sys.argv[1]).astype(float)
    s = symbols(sys.argv[2])
    up = 10  # 48 kHz
    u = np.zeros(len(s) * up)
    u[::up] = s
    y48 = np.convolve(u, rrc(0.5, up, 8), mode='same')
    best = None
    xz = x - x.mean()
    for ph in range(6):
        y = y48[ph::6]  # 8 kHz
        n = len(xz)
        c = np.fft.irfft(np.fft.rfft(y, len(y) + n) * np.conj(np.fft.rfft(xz, len(y) + n)))
        k = int(np.argmax(np.abs(c[:len(y) - n])))
        if best is None or abs(c[k]) > abs(best[0]):
            best = (c[k], ph, k)
    _, ph, k = best
    y = y48[ph::6][k:k + len(x)]
    A = np.vstack([y, np.ones_like(y)]).T
    (g, off), *_ = np.linalg.lstsq(A, x, rcond=None)
    res = x - A @ [g, off]
    print('aligned at symbol %.1f, gain %.1f units per symbol level (outer +-3: %.0f), offset %.0f units '
          '(%.0f Hz if +3 is 2400 Hz), fit SNR %.1f dB' % (k * 6 / up / 1.0 / 1, g, 3 * g, off, off / g * 800,
                                                       10 * np.log10(np.var(A @ [g, off]) / np.var(res))))


if __name__ == '__main__':
    main()
