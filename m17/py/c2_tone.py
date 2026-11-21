"""Codec 2 3200 frames that decode to one clean tone (the M17 test tone), found with the float model of the decoder
(c2_3200_model.py, checked against the reference decoder): voiced, the 7th harmonic of 142.97 Hz (fundamental index
34) at 1000.8 Hz, synthesised on the decoder's 512-point grid at exactly 1000 Hz (its phase 2.9 degrees off every
10 ms), and three LSPs 25 Hz apart around 1 kHz making the envelope's sharp peak; the other LSPs spread. A hill climb
over the LSP indexes keeps the other harmonics down.

  python m17/py/c2_tone.py out.c2 [--frames 750] [--energy 24]
"""
import argparse
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c2_3200_model as M  # noqa: E402

WO_INDEX = int(os.environ.get("C2_TONE_WO", "73"))


def cb_value(i, k):
    return 200 + 50 * (k - 7) if (3 <= i <= 5 and k >= 8) else 25 * (k + 1)


def gray(v):
    return v ^ (v >> 1)


def pack(e_index, ks):
    word = 0
    for v, width in [(1, 1), (1, 1), (WO_INDEX, 7), (e_index, 5)] + [(k, 5) for k in ks]:
        word = (word << width) | gray(v)
    return word.to_bytes(8, 'big')


def lsps(ks):
    out, hz = [], 0
    for i, k in enumerate(ks):
        hz += cb_value(i, k)
        out.append(hz)
    return out


def purity(frame, frames=24):
    d = M.Decoder()
    pcm = np.concatenate([d.decode(frame) for _ in range(frames)]).astype(float)[1280:]
    sp = np.abs(np.fft.rfft(pcm * np.hanning(len(pcm)))) ** 2
    f = np.fft.rfftfreq(len(pcm), 1 / 8000)
    tone = sp[(f > 985) & (f < 1015)].sum()
    return 10 * np.log10(tone / (sp.sum() - tone + 1e-9)), np.abs(pcm).max()


CLUSTER = (975, 1000, 1025) # three LSPs 25 Hz apart: the envelope's sharp peak at 1 kHz


def start():
    """the cluster with the other LSPs spread below and above it, the best of each split"""
    best = None
    for below in range(1, 8):
        above = 10 - 3 - below
        targets = ([CLUSTER[0] * (i + 1) / (below + 1) for i in range(below)] + list(CLUSTER) +
                   [CLUSTER[-1] + (3900 - CLUSTER[-1]) * (i + 1) / (above + 1) for i in range(above)])
        ks, prev = [], 0
        for i, t in enumerate(targets):
            k = min(range(32), key=lambda k: abs(prev + cb_value(i, k) - t))
            ks.append(k)
            prev += cb_value(i, k)
        if not all(c in lsps(ks) for c in CLUSTER):
            continue
        p, _ = purity(pack(24, ks))
        if best is None or p > best[0]:
            best = (p, ks)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out')
    ap.add_argument('--frames', type=int, default=750)
    ap.add_argument('--energy', type=int, default=20)
    ap.add_argument('--steps', type=int, default=300)
    args = ap.parse_args()
    random.seed(17)
    p0, ks = start()
    print('start %s %.1f dB' % (lsps(ks), p0))
    for it in range(args.steps): # the LSPs outside the cluster moved a step or two at a time
        trial = list(ks)
        i = random.randrange(10)
        trial[i] = min(31, max(0, trial[i] + random.choice((-2, -1, 1, 2))))
        if not all(c in lsps(trial) for c in CLUSTER):
            continue
        p, peak = purity(pack(args.energy, trial))
        if p > p0:
            p0, ks = p, trial
            print('%3d %s %.1f dB peak %.0f' % (it, lsps(ks), p0, peak), flush=True)
    frame = pack(args.energy, ks)
    p, peak = purity(frame)
    print('tone frame %s: LSPs %s, tone / the rest %.1f dB, peak %.0f' % (frame.hex(), lsps(ks), p, peak))
    open(args.out, 'wb').write(frame * args.frames)


if __name__ == '__main__':
    main()
