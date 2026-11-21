"""M17 receive against known content, live: the transmitter (m17_tx_source 1: the test sweep) keyed over the cable a
few times; after each transmission the receiver's counters and its last decoded stream frames (m17_stream_log, 64)
are read and each Codec 2 frame looked up in the PC encoder's frames of the same sweep. The transmitter's settings
are put back after. Both radios on the same frequency in M17 mode; how strong the signal is is up to the setup.

  python m17/py/m17_rx_check.py --tx COM23 --rx COM22 [--times 5] [--seconds 4] [--power low|channel]
"""
import argparse
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tools' / 'debug'))
from m17_tx_check import Mem, RX_FIELDS, sweep, restore  # noqa: E402
from radio import Radio  # noqa: E402

LOG = 64


def expected(frames):
    """the PC encoder's Codec 2 frames of the sweep: a set to look a frame up in"""
    out = ROOT / 'm17' / 'out'
    raw, c2 = out / 'rx_check.sweep.raw', out / 'rx_check.sweep.c2'
    raw.write_bytes(struct.pack('<%dh' % (160 * frames), *sweep(160 * frames)))
    subprocess.run([str(out / 'c2enc_run.exe'), str(raw), str(c2)], check=True, capture_output=True)
    d = c2.read_bytes()
    return {d[i:i + 8] for i in range(0, len(d), 8)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tx', required=True)
    ap.add_argument('--rx', required=True)
    ap.add_argument('--times', type=int, default=5)
    ap.add_argument('--seconds', type=float, default=4.0)
    ap.add_argument('--power', choices=('low', 'channel'), default='channel', help='low: 50 mW; channel: its own')
    a = ap.parse_args()
    ref = expected(int(a.seconds * 50) + 200)
    tx = Mem(a.tx, ('m17_tx_source', 'm17_tx_low_power', 'm17_tx_dev', 'm17_tx_stats'))
    saved = {k: tx.u32(k)[0] for k in ('m17_tx_source', 'm17_tx_low_power', 'm17_tx_dev')}
    tx.set('m17_tx_source', 1)
    tx.set('m17_tx_low_power', 1 if a.power == 'low' else 0)
    tx.close()
    total = {'sent': 0, 'got': 0, 'muted': 0, 'checked': 0, 'right': 0}
    try:
        for n in range(a.times):
            rx = Mem(a.rx, ('m17_radio_stats',))
            before = dict(zip(RX_FIELDS, rx.u32('m17_radio_stats', len(RX_FIELDS))))
            rx.close()
            r = Radio(a.tx)
            r.ptt(True)
            time.sleep(a.seconds)
            r.ptt(False)
            r.close()
            time.sleep(1.5)
            tx = Mem(a.tx, ('m17_tx_stats',))
            sent = tx.u32('m17_tx_stats', 4)[1]
            tx.close()
            rx = Mem(a.rx, ('m17_radio_stats', 'm17_stream_log', 'm17_stream_log_n'))
            after = dict(zip(RX_FIELDS, rx.u32('m17_radio_stats', len(RX_FIELDS))))
            k = rx.u32('m17_stream_log_n')[0]
            data = rx.link.read(rx.s['m17_stream_log'], LOG * 20)
            rx.close()
            got = after['streams'] - before['streams']
            muted = after['bad_frames'] - before['bad_frames']
            new = min(got, LOG)
            right = checked = 0
            for i in range(new):
                e = data[20 * ((k - new + i) % LOG):20 * ((k - new + i) % LOG) + 20]
                for h in range(2):
                    checked += 1
                    right += e[8 * h:8 * h + 8] in ref
            total['sent'] += sent
            total['got'] += got
            total['muted'] += muted
            total['checked'] += checked
            total['right'] += right
            print('transmission %d: %d stream frames sent, %d received (%.0f %%), %d muted; the last %d checked: Codec 2 '
                  'frames right %d of %d' % (n + 1, sent, got, 100.0 * got / max(1, sent), muted, new, right, checked))
    finally:
        restore(a.tx, saved)
    print('all: received %.1f %% of the stream frames, muted %d; Codec 2 frames right %.1f %% of those checked' % (
        100.0 * total['got'] / max(1, total['sent']), total['muted'], 100.0 * total['right'] / max(1, total['checked'])))


if __name__ == '__main__':
    main()
