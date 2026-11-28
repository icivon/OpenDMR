#!/usr/bin/env python3
"""Watch the volume potentiometer of the DM-36: the ADC raw value behind the knob.

The volume knob is a potentiometer on ADC channel 0; the firmware's adc_read_volume() returns the raw
value (0..1023, the knob's own travel using 0..~690). Turn the knob to one end and hold it there: the
smallest and the largest value of the run are printed at the end, which is what the level mapping needs.

The symbol address comes from the ELF of the firmware that is flashed (--elf; the DM-36 build by default).

  py dm36_volwatch.py                    20 s, a reading every 200 ms
  py dm36_volwatch.py --secs 30           longer, for holding both ends
  py dm36_volwatch.py --port /dev/ttyUSB1
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from dm32dbg import Radio  # noqa: E402

NM = os.path.join(REPO, 'toolchain', 'bin', 'csky-elfabiv2-nm')
ELF = os.path.join(REPO, 'src', 'build', 'opendmr_dm36.elf')


def symbol(elf, name):
    if not os.path.exists(elf):
        raise SystemExit('no ELF at %s: pass --elf of the flashed firmware' % elf)
    out = subprocess.run([NM, elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit('%s: no symbol %s (is it the ELF of the firmware flashed?)' % (elf, name))


def level_of(raw):
    # the same rounding as input.c: the knob's travel 0..690 onto the 16 levels
    return min(((raw * 15) + 345) // 690, 15)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='/dev/ttyUSB0')
    ap.add_argument('--elf', default=ELF)
    ap.add_argument('--symbol', default='adc_read_volume')
    ap.add_argument('--secs', type=float, default=20)
    ap.add_argument('--interval', type=float, default=0.2)
    args = ap.parse_args()

    addr = symbol(args.elf, args.symbol)
    radio = Radio(args.port)
    print('%s at %08x, %.0f s, a reading every %.0f ms' % (args.symbol, addr, args.secs, args.interval * 1000))

    low = high = None
    readings = 0
    end = time.time() + args.secs
    try:
        while time.time() < end:
            raw = radio.call(addr, []) & 0x3FF
            if low is None or raw < low:
                low = raw
            if high is None or raw > high:
                high = raw
            readings += 1
            print('%s raw=%3d level=%2d/15' % (time.strftime('%H:%M:%S'), raw, level_of(raw)), flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass

    if not readings:
        raise SystemExit('no readings')
    print('--- %d readings: raw min %d (level %d), max %d (level %d)'
          % (readings, low, level_of(low), high, level_of(high)))


if __name__ == '__main__':
    sys.exit(main())
