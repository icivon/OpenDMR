"""YSF radio to radio, hands off: the transmitter keyed for some seconds (its mic: silence or what it hears), the
receiver's counters before and after and its last frames (ysf_frame_log) read once at the end. Both radios in YSF
on the same simplex frequency (ysf/py/rig.py). The transmitter is not polled while it transmits.

  python ysf/py/tx_check.py --tx COM23 --rx COM22 [--seconds 3] [--power low|high] [--dev 1536] [--log]
"""
import argparse
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'm17' / 'py'))
sys.path.insert(0, str(ROOT / 'tools' / 'debug'))
from m17_tx_check import Mem  # noqa: E402
from radio import Radio  # noqa: E402

RX_FIELDS = ('halves missed_halves busy_us_max frames fich_bad dch_bad voice_frames rep_err rep_bits lost calls '
             'other_dgid pcm_drops underruns dec_us_max demod_syncs').split()
LOG = 64


def rx_stats(port):
    m = Mem(port, ('ysf_radio_stats',))
    v = dict(zip(RX_FIELDS, m.u32('ysf_radio_stats', len(RX_FIELDS))))
    m.close()
    return v


def frame_log(port, last):
    """the receiver's last frames: (fich 4 bytes, sync wrong | 0x80 guessed, 5 voice errors, 5 AMBE frames)"""
    m = Mem(port, ('ysf_frame_log', 'ysf_frame_log_n'))
    n = m.u32('ysf_frame_log_n')[0]
    raw = b''.join(m.link.read(m.s['ysf_frame_log'] + i, 896) for i in range(0, LOG * 56, 896))
    m.close()
    out = []
    for k in range(max(0, n - min(last, LOG)), n):
        e = raw[56 * (k % LOG):56 * (k % LOG) + 56]
        out.append((e[0:4], e[4], list(e[5:10]), [e[10 + 9 * j:19 + 9 * j] for j in range(5)]))
    return out


def fich_text(b):
    fi, fn, ft, dt, sc = b[0] >> 6, (b[1] >> 3) & 7, b[1] & 7, b[2] & 3, b[3] & 0x7F
    return '%s FN %d FT %d DT %d DG %d' % (('HC', 'CC', 'TC', 'T?')[fi], fn, ft, dt, sc)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tx', required=True)
    ap.add_argument('--rx', required=True)
    ap.add_argument('--seconds', type=float, default=3.0)
    ap.add_argument('--power', choices=('low', 'high'), default='low')
    ap.add_argument('--dev', type=int)
    ap.add_argument('--log', action='store_true', help='print the receiver frame log')
    a = ap.parse_args()

    tx = Mem(a.tx, ('ysf_tx_low_power', 'ysf_tx_dev', 'ysf_tx_stats'))
    saved = {k: tx.u32(k)[0] for k in ('ysf_tx_low_power', 'ysf_tx_dev')}
    tx.set('ysf_tx_low_power', 1 if a.power == 'low' else 0)
    if a.dev:
        tx.set('ysf_tx_dev', a.dev)
    dev = tx.u32('ysf_tx_dev')[0]
    tx.close()
    before = rx_stats(a.rx)
    try:
        r = Radio(a.tx)
        r.ptt(True)
        time.sleep(a.seconds)
        r.ptt(False)
        r.close()
    finally:
        time.sleep(1.0)
        tx = Mem(a.tx, tuple(saved) + ('ysf_tx_stats',))
        calls, frames, enc_us, underruns = tx.u32('ysf_tx_stats', 4)
        for k, v in saved.items():
            tx.set(k, v)
        tx.close()
    time.sleep(0.5)
    after = rx_stats(a.rx)
    d = {f: after[f] - before[f] for f in RX_FIELDS}
    print('transmitter: dev %d, %d frames, the longest AMBE encoding %.1f ms, underruns %d' % (
        dev, frames, enc_us / 1000, underruns))
    print('receiver: ' + ', '.join('%s +%d' % (f, d[f]) for f in RX_FIELDS
                                   if f not in ('busy_us_max', 'dec_us_max', 'halves', 'missed_halves')))
    print('receiver: repetition BER %.2f %%, busy max %.1f ms, decode max %.1f ms' % (
        100.0 * d['rep_err'] / max(d['rep_bits'], 1), after['busy_us_max'] / 1000, after['dec_us_max'] / 1000))
    if a.log:
        for fich, sw, errs, _ in frame_log(a.rx, d['frames']):
            print('  %s sync %d%s errs %s' % (fich_text(fich), sw & 0x7F, ' (guessed)' if sw & 0x80 else '', errs))
    return d


if __name__ == '__main__':
    main()
