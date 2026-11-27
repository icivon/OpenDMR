#!/usr/bin/env python3
"""Live RX check for the DM-36: FD6818 baseband routing + DMR soft-demod stats.

Run this in one terminal, then switch the radio to a DMR channel and listen to
a known-good DMR transmission. What to look for:

  - On a DMR channel FD6818 reg 0x50 must be BB20 (raw baseband to the HR_C7000)
    and 0x43 must be 0048 (DMR bandwidth). If 0x50 stays 0000 / 0x43 stays 3028,
    the radio never left the FM setup -> no DMR sync, no ID, no audio.
  - syncs / voice_bursts must increase while someone talks. halves increasing
    with syncs staying 0 means RF is received but the DMR demod never locks.

  .venv/bin/python tools/debug/dm36_rfwatch.py --port /dev/ttyUSB0

Requires the ELF of the firmware currently flashed (DM32_ELF overrides it).
"""
import argparse
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dm32dbg  # noqa: E402

ELF = os.environ.get("DM32_ELF", os.path.join(HERE, "..", "..", "src", "build", "opendmr_dm36.elf"))


def symbols(elf):
    out = {}
    r = subprocess.run([dm32dbg.NM, elf], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            out[p[2]] = int(p[0], 16)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("DM32_PORT", "/dev/ttyUSB0"))
    ap.add_argument("--seconds", type=int, default=0, help="0 = until Ctrl-C")
    args = ap.parse_args()

    sym = symbols(ELF)
    read_addr = sym.get("trx_spi_read")
    if read_addr is None:
        sys.exit("trx_spi_read not found in %s (wrong ELF?)" % ELF)
    # dmr_soft_stats: halves, missed, busy_us_max, bursts, syncs, lost, relocks, ...
    stats_addr = sym.get("dmr_soft_stats")
    if stats_addr is None:
        sys.exit("dmr_soft_stats not found in %s (wrong ELF?)" % ELF)

    radio = dm32dbg.Radio(args.port)
    radio.p.reset_input_buffer()
    print("watching FD6818 + DMR stats on %s (Ctrl-C to stop)" % args.port, flush=True)
    end = time.time() + args.seconds if args.seconds else None
    try:
        while end is None or time.time() < end:
            try:
                reg50 = radio.call(read_addr, [0x50]) & 0xFFFF
                reg43 = radio.call(read_addr, [0x43]) & 0xFFFF
                raw = radio.mem_read(stats_addr, 32)
                halves, missed, busy, bursts, syncs, lost, relocks = struct.unpack_from("<7I", raw)
                print("%s  0x50=%04X 0x43=%04X  halves=%d bursts=%d syncs=%d lost=%d"
                      % (time.strftime("%H:%M:%S"), reg50, reg43, halves, bursts, syncs, lost), flush=True)
            except Exception:
                print("  (no reply - radio off / transmitting?)", flush=True)
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
