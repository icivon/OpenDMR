"""A YSF transmission recorded as the software receivers get it (the FD6818's discriminator at 8 kHz, FM_BANDWIDTH 0x0F):
the transmitter (in YSF) keyed by the test link, the receiver in M17 mode (its raw recording tap, m17_radio.c
m17_raw_tap) takes 12000 samples (1.5 s) from --after seconds on. Saved to ysf/data/cap/<name>.raw (16-bit, 8 kHz);
decode with  python ysf/py/demod.py ysf/data/cap/<name>.raw  or  ysf/c/ysf_test.exe rx ysf/data/cap/<name>.raw

  python ysf/py/capture.py name [--tx COM23] [--rx COM22] [--after 1.0] [--seconds 3] [--low]
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'debug'))
from dm32dbg import ELF, Radio, Symbols  # noqa: E402

N = 12000


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('name')
    ap.add_argument('--tx', default='COM23')
    ap.add_argument('--rx', default='COM22')
    ap.add_argument('--after', type=float, default=1.0)
    ap.add_argument('--seconds', type=float, default=3.0)
    ap.add_argument('--low', action='store_true', help='the transmitter at 50 mW')
    a = ap.parse_args()
    s = Symbols(ELF)
    tx = Radio(a.tx)
    tx.value_write(s.resolve('ysf_tx_low_power'), 4, 1 if a.low else 0)
    rx = Radio(a.rx)
    rx.value_write(s.resolve('m17_raw_hold'), 4, 1)  # the tap stopped
    tx.call(s.resolve('radio_transmit'), [1])
    time.sleep(a.after)
    rx.value_write(s.resolve('m17_raw_tap_n'), 4, 0)
    rx.value_write(s.resolve('m17_raw_hold'), 4, N + 1)  # N samples from now, then it stops
    time.sleep(max(0.0, a.seconds - a.after))
    tx.call(s.resolve('radio_transmit'), [0])
    time.sleep(1.5)
    n = rx.value_read(s.resolve('m17_raw_tap_n'), 4)
    tap = s.resolve('m17_raw_tap')
    raw = b''
    while len(raw) < 2 * N:
        raw += rx.mem_read(tap + len(raw), min(240, 2 * N - len(raw)))
    out = os.path.join(HERE, '..', 'data', 'cap')
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, a.name + '.raw'), 'wb').write(raw[:2 * min(n, N)])
    print('%s: %d samples' % (a.name, min(n, N)))


if __name__ == '__main__':
    main()
