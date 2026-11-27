"""Record what the receiver (in M17 mode: its raw tap) hears now, 12000 samples (1.5 s), into ysf/data/cap/<name>.raw:
for signals from elsewhere (the hotspot's test transmitter).

  python ysf/py/record.py name [--rx COM22]
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'debug'))
from dm32dbg import ELF, Radio, Symbols  # noqa: E402

N = 12000

ap = argparse.ArgumentParser()
ap.add_argument('name')
ap.add_argument('--rx', default='COM22')
a = ap.parse_args()
s = Symbols(ELF)
rx = Radio(a.rx)
rx.value_write(s.resolve('m17_raw_tap_n'), 4, 0)
rx.value_write(s.resolve('m17_raw_hold'), 4, N + 1)
time.sleep(N / 8000 + 0.5)
n = rx.value_read(s.resolve('m17_raw_tap_n'), 4)
tap = s.resolve('m17_raw_tap')
raw = b''
while len(raw) < 2 * N:
    raw += rx.mem_read(tap + len(raw), min(240, 2 * N - len(raw)))
out = os.path.join(HERE, '..', 'data', 'cap')
os.makedirs(out, exist_ok=True)
open(os.path.join(out, a.name + '.raw'), 'wb').write(raw[:2 * min(n, N)])
print('%s: %d samples' % (a.name, min(n, N)))
