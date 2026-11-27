"""Symbol errors of the firmware's demodulator (ysf/c/ysf_test.exe) on a recording: its dibits cut into frames at the
syncs, each frame whose FICH and DCH decode re-encoded (ysf.py) and compared where the content is known (sync, FICH,
DCH, the voice's tripled bits; not the 22 unprotected voice bits).

  python ysf/py/ser.py ysf/data/cap/weak1.raw [...]
"""
import os
import subprocess
import sys

import numpy as np

import ysf

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', 'c', 'ysf_test.exe')
SYNC_D = [(ysf.SYNC >> (38 - 2 * k)) & 3 for k in range(20)]
UNPROT = set()
for j in range(5):
    for i in range(81, 104):
        UNPROT.add(240 + 144 * j + 40 + 4 * (i % 26) + i // 26)


def known_mask():
    m = np.ones(960, bool)
    for b in UNPROT:
        m[b] = False
    return m


def run(path):
    out = path + '.dibits'
    r = subprocess.run([EXE, 'rx', path, os.devnull, out], capture_output=True, text=True)
    d = np.fromfile(out, np.uint8)
    starts = [i for i in range(len(d) - 480) if np.sum(d[i:i + 20] != SYNC_D) <= 4]
    frames, wrong_bits, bits, wrong_sym, syms = 0, 0, 0, 0, 0
    mask = known_mask()
    last = -1000
    for s in starts:
        if s - last < 400:
            continue
        last = s
        fr = d[s:s + 480]
        rb = []
        for x in fr:
            rb += [int(x) >> 1, int(x) & 1]
        p = ysf.parse_frame(rb)
        f = p['fich']
        if not f or f['fi'] != ysf.FI_CC or f['dt'] != ysf.DT_VD2 or p['dch'][0] is None:
            continue
        ref = ysf.frame_vd2(f['fn'], f['ft'], p['dch'][0], [v for v, _ in p['voice']], mr=f['mr'], cm=f['cm'],
                            sc=f['sc'])
        rb, ref = np.array(rb), np.array(ref)
        frames += 1
        wrong_bits += int(np.sum((rb != ref) & mask))
        bits += int(mask.sum())
        sm = mask[0::2] & mask[1::2]
        ws = (rb[0::2] != ref[0::2]) | (rb[1::2] != ref[1::2])
        wrong_sym += int(np.sum(ws & sm))
        syms += int(sm.sum())
    print('%s: %s | %d frames checked, bit errors %.2f %%, symbol errors %.2f %%' % (
        os.path.basename(path), r.stdout.strip().splitlines()[-1], frames, 100.0 * wrong_bits / max(bits, 1),
        100.0 * wrong_sym / max(syms, 1)))


for p in sys.argv[1:]:
    run(p)
