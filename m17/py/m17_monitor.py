"""The M17 receiver's counters (m17_radio_stats) and its current call over the cable, read once (don't poll while a
call is being timed). With --tap FILE also the last 4096 dibits given to m17_core.c (4: erased), one a byte.

  python m17/py/m17_monitor.py --port COM22 [--tap out.dibits]
"""
import argparse
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "vocoder" / "py"))
from radio_link import Link  # noqa: E402

FIELDS = ("halves syncs lost streams bad_frames lsfs eots c2_frames pcm_drops underruns calls decode_us_max "
          "busy_us_max gain_sum").split()
RAW_TAP = 12000  # samples (src/services/m17_radio.c M17_RAW_TAP)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--tap", type=Path)
    ap.add_argument("--raw", type=Path, help="the recording around the last call's start (raw 16-bit 8 kHz); then "
                    "the tap taking again")
    args = ap.parse_args()
    link = Link.__new__(Link)  # no trampoline needed: reads only
    import serial
    import radio_link
    link.p = serial.Serial(args.port, 115200, timeout=0.5)
    link.last_keepalive = 1e18
    link.s = radio_link.symbols(("m17_radio_stats", "m17_tap", "m17_tap_n", "m17_raw_tap", "m17_raw_tap_n", "m17_raw_rearm"))
    try:
        raw = link.read(link.s["m17_radio_stats"], 4 * len(FIELDS))
        print(", ".join("%s %d" % kv for kv in zip(FIELDS, struct.unpack("<%dI" % len(FIELDS), raw))))
        if args.raw:
            n = struct.unpack("<I", link.read(link.s["m17_raw_tap_n"], 4))[0]
            size = 2 * RAW_TAP
            data = b"".join(link.read(link.s["m17_raw_tap"] + i, min(1024, size - i)) for i in range(0, size, 1024))
            k = (n % RAW_TAP) * 2
            args.raw.write_bytes(data[k:] + data[:k] if n >= RAW_TAP else data[:2 * n])
            link.write(link.s["m17_raw_rearm"], struct.pack("<I", 1))
            print("raw: %d samples (%d in all)" % (min(n, RAW_TAP), n))
        if args.tap:
            n = struct.unpack("<I", link.read(link.s["m17_tap_n"], 4))[0]
            data = b"".join(link.read(link.s["m17_tap"] + i, 1024) for i in range(0, 4096, 1024))
            k = n % 4096
            args.tap.write_bytes(data[k:] + data[:k] if n >= 4096 else data[:n])
            print("tap: %d dibits (%d in all)" % (min(n, 4096), n))
    finally:
        link.p.close()


if __name__ == "__main__":
    main()
