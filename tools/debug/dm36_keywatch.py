#!/usr/bin/env python3
"""Live key monitor for the DM-36: prints each key press as it happens.

This calls keys_read_keypad() / keys_read_buttons() on the radio over the cable
and decodes the raw matrix bits with the same BOARD_KEY_INDEX() map as the
firmware, so what you see is exactly what the firmware's input task sees.

  .venv/bin/python tools/debug/dm36_keywatch.py --port /dev/ttyUSB0

Requires the ELF of the firmware currently flashed: by default src/build/opendmr_dm36.elf,
override with DM32_ELF. The radio must be on and running OpenDMR (not the bootloader).
PTT cannot be watched: pressing it stops the debug link (the radio transmits), so it is
left out here; the pin is 43 from the stock firmware analysis.

PTT is also masked by the firmware whenever the cable link is busy
(keys.c: ptt_down && !cable_link_is_busy(), CPS_LINK_BUSY_MS = 1000). A polling
script like this keeps the link busy, so PTT will never show here by design --
it is not evidence that the PTT pin is wrong. To check PTT, unplug the cable
and transmit normally.
"""
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dm32dbg  # noqa: E402

ELF = os.environ.get("DM32_ELF", os.path.join(HERE, "..", "..", "src", "build", "opendmr_dm36.elf"))

# keys.h / ui.h bit numbers -> names, from the DM-36 board map
KEY_NAMES = {
    0: "GREEN/Menu", 1: "3", 2: "2", 3: "1", 4: "LEFT", 5: "6", 6: "5", 7: "4",
    8: "RIGHT", 9: "9", 10: "8", 11: "7", 12: "RED/Back", 13: "HASH(#)", 14: "0",
    15: "STAR(*)", 16: "UP", 17: "DOWN",
}

# Mirrors BOARD_KEY_INDEX() of src/board/board_dm36.h -- keep the two in sync.
DM36_MAP = {0: 15, 4: 14, 8: 13, 12: 0, 13: 16, 14: 17, 15: 12}


def symbols(elf):
    out = {}
    r = subprocess.run([dm32dbg.NM, elf], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            out[p[2]] = int(p[0], 16)
    return out


def decode(mask, buttons):
    # The mask bits are the logical key numbers (BOARD_KEY_INDEX output), i.e. what
    # input.c sees. Show that name first, plus the matrix position it came from.
    inverse = {v: k for k, v in DM36_MAP.items()}
    names = []
    for key in range(18):
        if mask & (1 << key):
            raw = inverse.get(key)
            names.append("%s (matrix %s)" % (KEY_NAMES.get(key, str(key)),
                                              raw if raw is not None else "same"))
    if buttons & 0x01:
        names.append("SK1")
    if buttons & 0x02:
        names.append("SK2")
    if buttons & 0x04:
        names.append("PTT")
    return names


def main():
    import argparse

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("DM32_PORT", "/dev/ttyUSB0"))
    ap.add_argument("--poll", type=float, default=0.12, help="seconds between reads")
    args = ap.parse_args()

    sym = symbols(ELF)
    addr_kp = sym.get("keys_read_keypad")
    addr_btn = sym.get("keys_read_buttons")
    if addr_kp is None:
        sys.exit("keys_read_keypad not found in %s (wrong ELF?)" % ELF)

    radio = dm32dbg.Radio(args.port)
    radio.p.reset_input_buffer()
    print("listening on %s ... (Ctrl-C to stop)" % args.port, flush=True)
    last_kp = None
    last_btn = None
    try:
        while True:
            try:
                kp = radio.call(addr_kp, [])
                btn = radio.call(addr_btn, []) if addr_btn else 0
            except Exception:
                print("  (no reply - is the radio on and running OpenDMR?)", flush=True)
                time.sleep(1.0)
                continue
            if kp != last_kp or btn != last_btn:
                stamp = time.strftime("%H:%M:%S")
                keys = decode(kp, btn)
                print("%s  keypad=0x%X buttons=0x%02X  %s"
                      % (stamp, kp, btn, ", ".join(keys) if keys else "-released-"), flush=True)
                last_kp, last_btn = kp, btn
            time.sleep(args.poll)
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
