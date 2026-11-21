"""DMR voice from a discriminator recording (8 kHz) by our software sync and demodulation (dmr_lab.py): the AMBE+2
frames of every voice burst, judged as the firmware's decoder judges them (vocoder/c/ambe: C0 + C1 Golay
corrections over 3: a bad frame, the last one repeated: 'R'), to compare with the frames the HR_C7000 hands over
(RX_CAPTURE, tools/debug/rx_capture.py) on the same signal.

A direct mode (MS) transmission: a burst every 60 ms, the superframe's burst A with the voice sync, B - F (EMB in the
middle) timed from it.

  python dmr_soft/py/dmr_frames.py rec1.raw [...] [--threshold 0.25]
  python dmr_soft/py/dmr_frames.py --bit capture.bit     (the same statistics of captured frames)
"""
import argparse
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'vocoder' / 'py'))
import dmr_lab as L  # noqa: E402
from ambe_frame import decode_frame  # noqa: E402

BURST = 2 * 144 * L.SPS  # 60 ms


def frame_errors(bits72):
    _, e0, e1 = decode_frame(list(bits72))
    return e0 + e1


def burst_frames(dibits):
    """132 dibits -> the 3 frames of 72 bits (the second one split around the middle 48 bits)"""
    bits = []
    for d in dibits:
        bits += [(d >> 1) & 1, d & 1]
    voice = bits[:108] + bits[156:]
    return [voice[0:72], voice[72:144], voice[144:216]]


def stats(errs, seconds=None):
    errs = np.array(errs)
    bad = int(np.sum(errs > 3))
    s = '%d frames, bad (C0 + C1 > 3) %d (%.1f %%), errors per frame: ' % (len(errs), bad, 100.0 * bad / max(1, len(errs)))
    s += ', '.join('%d:%d' % (k, int(np.sum(errs == k))) for k in range(0, 8))
    if seconds:
        s += '; %.1f frames a second (50 sent)' % (len(errs) / seconds)
    return s


def from_recording(paths, threshold):
    level = {3: 1, 1: 0, -1: 2, -3: 3}
    errs = []
    seconds = 0.0
    for path in paths:
        x = np.fromfile(path, '<i2').astype(float)
        x -= np.mean(x)
        seconds += len(x) / 8000
        syncs = [s for s in L.find_syncs(x, threshold) if s[1] == 'MS voice']
        done = set()
        for t0, name, pol, g, dc, e in syncs:
            for k in range(-5, 6):  # the bursts of this superframe and the one before (if no sync of their own)
                t = t0 + k * BURST
                slot = round(t / BURST)
                if slot in done or t < 4 or t + 133 * L.SPS > len(x) - 4:
                    continue
                near = min(syncs, key=lambda s: abs(s[0] - t))
                if near[0] != t0:
                    continue  # another sync is nearer: its timing
                done.add(slot)
                z = (L.interp(x, t + np.arange(132) * L.SPS) - dc) / g
                sym = np.where(z > 2, 3, np.where(z > 0, 1, np.where(z > -2, -1, -3)))
                frames = burst_frames([level[int(s)] for s in sym])
                errs += [frame_errors(f) for f in frames]
        print('%s: %d voice syncs' % (path, len(syncs)))
    print('software: ' + stats(errs, seconds))


def decide(z):
    return np.where(z > 2, 3, np.where(z > 0, 1, np.where(z > -2, -1, -3)))


def burst_dd(x, t, g, dc, search, refit):
    """a burst at about t: the best time within +-search samples (decision directed: the smallest mean squared
    distance of the 132 symbols to their decisions), gain and DC fitted again to the decisions (refit); the symbols"""
    best = None
    for dt in (np.arange(-search, search + 1e-9, 0.125) if search else (0.0,)):
        y = L.interp(x, t + dt + np.arange(132) * L.SPS)
        gg, dd = g, dc
        for _ in range(refit + 1):
            z = (y - dd) / gg
            sym = decide(z)
            if refit:
                A = np.vstack([sym, np.ones(132)]).T
                (gg, dd), *_ = np.linalg.lstsq(A, y, rcond=None)
                if gg <= 0:
                    gg, dd = g, dc
        z = (y - dd) / gg
        sym = decide(z)
        e = np.mean((z - sym) ** 2)
        if best is None or e < best[0]:
            best = (e, sym, dt)
    return best


def from_recording_v2(paths, threshold, search, refit):
    level = {3: 1, 1: 0, -1: 2, -3: 3}
    errs = []
    seconds = 0.0
    for path in paths:
        x = np.fromfile(path, '<i2').astype(float)
        x -= np.mean(x)
        seconds += len(x) / 8000
        syncs = [s for s in L.find_syncs(x, threshold) if s[1] == 'MS voice']
        if not syncs:
            print('%s: no voice sync' % path)
            continue
        # one time line for the whole recording: burst n at a + b n (b about BURST), fitted to the syncs (every 6th)
        t0 = syncs[0][0]
        n = np.array([round((s[0] - t0) / BURST) for s in syncs])
        ts = np.array([s[0] for s in syncs])
        if len(syncs) >= 2:
            b, a = np.polyfit(n, ts, 1)
            keep = np.abs(ts - (a + b * n)) < 1.5
            if keep.sum() >= 2:
                b, a = np.polyfit(n[keep], ts[keep], 1)
        else:
            a, b = t0, BURST
        g = float(np.median([s[3] for s in syncs]))
        dc = float(np.median([s[4] for s in syncs]))
        k_lo = int(np.ceil((4 - a) / b))
        k_hi = int(np.floor((len(x) - 4 - 133 * L.SPS - a) / b))
        used = 0
        for k in range(k_lo, k_hi + 1):
            e, sym, dt = burst_dd(x, a + b * k, g, dc, search, refit)
            if e > 0.5:  # no burst there (the transmission's ends)
                continue
            used += 1
            frames = burst_frames([level[int(v)] for v in sym])
            errs += [frame_errors(f) for f in frames]
        print('%s: %d voice syncs, time line %.4f samples a burst (%.4f nominal), %d bursts' % (
            path, len(syncs), b, BURST, used))
    print('software v2: ' + stats(errs, seconds))


def from_bit(path):
    d = Path(path).read_bytes()
    errs = []
    for i in range(len(d) // 9):
        b = d[9 * i:9 * i + 9]
        errs.append(frame_errors([(b[j // 8] >> (7 - j % 8)) & 1 for j in range(72)]))
    print('captured: ' + stats(errs))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--bit', action='store_true')
    ap.add_argument('--threshold', type=float, default=0.25)
    ap.add_argument('--v2', action='store_true', help='one time line, decision directed timing / gain / DC')
    ap.add_argument('--search', type=float, default=1.0, help='v2: samples searched around each burst')
    ap.add_argument('--refit', type=int, default=1, help='v2: decision directed gain / DC fits')
    a = ap.parse_args()
    if a.bit:
        for f in a.files:
            from_bit(f)
    elif a.v2:
        from_recording_v2(a.files, a.threshold, a.search, a.refit)
    else:
        from_recording(a.files, a.threshold)


if __name__ == '__main__':
    main()
