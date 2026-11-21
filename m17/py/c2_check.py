"""Compare decoded speech with the reference decoder's: waveform difference, and the spectral distance per 10 ms
(phase independent: unvoiced harmonics have random phases).

  python m17/py/c2_check.py ref.raw ours.raw
"""
import sys

import numpy as np


def frames_db(x):
    n = len(x) // 80 * 80
    f = x[:n].reshape(-1, 80) * np.hanning(80)
    s = np.abs(np.fft.rfft(f, 256, axis=1)) ** 2
    return 10 * np.log10(s + 1e2)


def main(ref_path, path):
    ref = np.fromfile(ref_path, '<i2').astype(float)
    x = np.fromfile(path, '<i2').astype(float)
    n = min(len(ref), len(x))
    ref, x = ref[:n], x[:n]
    d = ref - x
    sd = np.sqrt(np.mean((frames_db(ref) - frames_db(x)) ** 2, axis=1))
    loud = np.sqrt(np.mean(ref[:len(sd) * 80].reshape(-1, 80) ** 2, axis=1)) > 100
    print('%-30s rms ref %6.0f ours %6.0f  diff %5.1f dB  spectral distance median %.2f dB, 90%% %.2f dB '
          '(loud frames)' % (path[-30:], np.sqrt(np.mean(ref ** 2)), np.sqrt(np.mean(x ** 2)),
                             10 * np.log10(np.mean(d ** 2) / np.mean(ref ** 2)), np.median(sd[loud]),
                             np.percentile(sd[loud], 90)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
