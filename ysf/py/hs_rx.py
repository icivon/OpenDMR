"""Our YSF receiver on frames sent by the hotspot (pistar-signal-gen with YSF: p25tx-ctl start <file>): the receiver's
counters before and after, its last frames (ysf_frame_log) against the AMBE frames sent (a .bit file of the same
voice: 9 bytes a frame). PISTAR_HOST / PISTAR_PASS from the environment. The receiver in YSF on the hotspot's TX.

  python ysf/py/hs_rx.py ysf_ftm400.txt ysf/data/rec/dn.bit [--rx COM22] [--seconds 22] [--level 128]
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'nxdn', 'py'))
import pi  # noqa: E402
from tx_check import RX_FIELDS, frame_log, rx_stats  # noqa: E402


def hotspot(c, cmd):
    return pi.run(c, "echo '%s' | sudo -S %s 2>&1 | grep -v password" % (os.environ['PISTAR_PASS'], cmd))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('file')
    ap.add_argument('ref')
    ap.add_argument('--rx', default='COM22')
    ap.add_argument('--seconds', type=float, default=22)
    ap.add_argument('--level', type=int, default=128)
    ap.add_argument('--freq', type=int, default=439000000)
    a = ap.parse_args()
    ref = open(a.ref, 'rb').read()
    ref_set = {ref[i:i + 9] for i in range(0, len(ref), 9)}
    c = pi.connect()
    env = 'P25TX_ARGS=/usr/local/p25tx/files/%s --rx 434000000 --tx %d --gap 0 --max %d --level %d' % (
        a.file, a.freq, int(a.seconds) + 5, a.level)
    before = rx_stats(a.rx)
    print(hotspot(c, "sh -c 'mkdir -p /run/p25tx && echo \"%s\" > /run/p25tx/env && systemctl start p25tx'" % env).strip())
    time.sleep(a.seconds)
    after = rx_stats(a.rx)
    print(hotspot(c, '/usr/local/p25tx/p25tx-ctl stop').strip())
    print(pi.run(c, 'journalctl -u p25tx -n 6 --no-pager -o cat').strip())
    d = {f: after[f] - before[f] for f in RX_FIELDS}
    print('receiver: ' + ', '.join('%s +%d' % (f, d[f]) for f in RX_FIELDS
                                   if f not in ('busy_us_max', 'dec_us_max', 'halves')))
    print('receiver: repetition BER %.2f %%, busy max %.1f ms' % (100.0 * d['rep_err'] / max(d['rep_bits'], 1),
                                                                    after['busy_us_max'] / 1000))
    log = frame_log(a.rx, min(d['frames'], 64))
    same = total = 0
    for fich, sw, errs, voice in log:
        for j, v in enumerate(voice):
            if errs[j] != 0xFF:
                total += 1
                same += v in ref_set
    print('the last %d frames: %d voice frames, %d identical to frames sent' % (len(log), total, same))


if __name__ == '__main__':
    main()
