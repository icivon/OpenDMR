#!/usr/bin/env python3
"""The DM-36's signal readings and backlight, printed as they change.

Run it while a second radio transmits (or a call comes in): "sig" should rise, "lit" should go to 1 and the S meter
follow. A "sig" that rises with "lit" still 0 points at the backlight; a "sig" that stays flat points at the receiver's
readings (the meter and the screen both take their level from signal_shown).
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dm32dbg import Radio, Symbols

FIELDS = ("signal_shown", "signal_raw", "noise_raw", "digital_carrier", "lit", "light_left_ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--elf", required=True, help="the ELF of the firmware that is flashed")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--interval", type=float, default=0.2)
    args = parser.parse_args()

    symbols = Symbols(args.elf)
    addresses = {name: symbols.resolve(name) for name in FIELDS}
    radio = Radio(args.port)
    started = time.time()

    print("  time  sig raw noi dmr lit  left")
    while time.time() - started < args.seconds:
        value = {name: radio.mem_read(address, 4) for name, address in addresses.items()}
        print("%6.2f  %3d %3d %3d  %d   %d %5d" % (
            time.time() - started,
            value["signal_shown"][0],
            value["signal_raw"][0],
            value["noise_raw"][0],
            value["digital_carrier"][0],
            value["lit"][0],
            int.from_bytes(value["light_left_ms"], "little")), flush=True)
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
