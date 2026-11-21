"""M17 transmit, end to end with two DM-32s (debug builds, src/build's ELF), hands off: the transmitter sends the test
sweep (m17_tx_source 1) through its Codec 2 encoder; the receiver's dibit tap (the last 4096) is decoded on the PC
(m17/out/m17_core_test.exe) and its Codec 2 frames compared with the PC encoder's (m17/out/c2enc_run.exe) for the same
sweep. Both radios on the same simplex frequency in M17 mode. The transmitter is not polled while it transmits.

  python m17/py/m17_tx_check.py --tx COM22 --rx COM23 [--seconds 3] [--power low|high] [--source sweep|mic]
"""
import argparse
from pathlib import Path
import math
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "vocoder" / "py"))
sys.path.insert(0, str(ROOT / "tools" / "debug"))
import radio_link  # noqa: E402
from radio import Radio  # noqa: E402

OUT = ROOT / "m17" / "out"
RX_FIELDS = ("halves syncs lost streams bad_frames lsfs eots c2_frames pcm_drops underruns calls decode_us_max "
             "busy_us_max gain_sum").split()
SWEEP_LO, SWEEP_HI, SWEEP_STEP = 161061273, 1610612736, 90597


def sintab():
    return [int(round(32767 * math.sin(2 * math.pi * i / 1024))) for i in range(1024)]


def sweep(samples):
    """the firmware's test sweep (src/services/m17_radio.c), sample for sample"""
    tab, out, tone, step = sintab(), [], 0, SWEEP_LO
    for _ in range(samples):
        out.append(tab[tone >> 22] >> 2)
        tone = (tone + step) & 0xFFFFFFFF
        step = SWEEP_LO if step + SWEEP_STEP > SWEEP_HI else step + SWEEP_STEP
    return out


class Mem:
    def __init__(self, port, names):
        import serial
        self.link = radio_link.Link.__new__(radio_link.Link)  # reads and writes only
        self.link.p = serial.Serial(port, 115200, timeout=0.5)
        self.link.last_keepalive = 1e18
        self.s = radio_link.symbols(names)

    def u32(self, name, n=1):
        return struct.unpack("<%dI" % n, self.link.read(self.s[name], 4 * n))

    def set(self, name, value):
        self.link.write(self.s[name], struct.pack("<I", value))

    def close(self):
        self.link.p.close()


def restore(port, saved):
    """the transmitter's settings as they were (the sweep and 50 mW left on would be what the user hears next)"""
    tx = Mem(port, tuple(saved))
    for k, v in saved.items():
        tx.set(k, v)
    tx.close()
    print("transmitter's settings restored: " + ", ".join("%s %d" % kv for kv in saved.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tx", required=True)
    ap.add_argument("--rx", required=True)
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--power", choices=("low", "high"), default="low")
    ap.add_argument("--source", choices=("sweep", "mic"), default="sweep")
    ap.add_argument("--dev", type=int, help="m17_tx_dev (MODEM_MOD_SCALE_ONE 1024: P25's level)")
    args = ap.parse_args()

    tx = Mem(args.tx, ("m17_tx_source", "m17_tx_low_power", "m17_tx_stats", "m17_tx_dev"))
    saved = {k: tx.u32(k)[0] for k in ("m17_tx_source", "m17_tx_low_power", "m17_tx_dev")}
    tx.set("m17_tx_source", 1 if args.source == "sweep" else 0)
    tx.set("m17_tx_low_power", 1 if args.power == "low" else 0)
    if args.dev:
        tx.set("m17_tx_dev", args.dev)
    dev = tx.u32("m17_tx_dev")[0]
    tx.close()
    rx = Mem(args.rx, ("m17_radio_stats", "m17_tap", "m17_tap_n"))
    before = dict(zip(RX_FIELDS, rx.u32("m17_radio_stats", len(RX_FIELDS))))
    rx.close()

    try:
        r = Radio(args.tx)
        r.ptt(True)
        time.sleep(args.seconds)
        r.ptt(False)
        r.close()
    finally:
        time.sleep(1.5)
        restore(args.tx, saved)
    time.sleep(1.5)  # the last frames and the EoT sent, the receiver done

    rx = Mem(args.rx, ("m17_radio_stats", "m17_tap", "m17_tap_n"))
    after = dict(zip(RX_FIELDS, rx.u32("m17_radio_stats", len(RX_FIELDS))))
    n = rx.u32("m17_tap_n")[0]
    data = b"".join(rx.link.read(rx.s["m17_tap"] + i, 1024) for i in range(0, 4096, 1024))
    rx.close()
    k = n % 4096
    tap = data[k:] + data[:k] if n >= 4096 else data[:n]
    tx = Mem(args.tx, ("m17_tx_stats", "c2enc_prof"))
    calls, frames, enc_us, underruns = tx.u32("m17_tx_stats", 4)
    prof = tx.u32("c2enc_prof", 6) if "c2enc_prof" in tx.s else None
    if prof:
        tx.set("c2enc_prof", 0)
        tx.link.write(tx.s["c2enc_prof"], bytes(24))
    tx.close()

    print("transmitter: dev %d, %d stream frames, the longest Codec 2 frame's encoding %.2f ms, underruns %d"
          % (dev, frames, enc_us / 1000, underruns))
    if prof and frames:
        print("encoding, ms a 20 ms frame (since the last check): " + ", ".join("%s %.2f" % (name, t / (2 * frames) / 1000)
              for name, t in zip(("pitch filter", "pitch spectrum", "spectrum", "refinement", "voicing", "LPC"), prof)))
    print("receiver: " + ", ".join("%s +%d" % (f, after[f] - before[f]) for f in RX_FIELDS
                                   if f not in ("decode_us_max", "busy_us_max", "gain_sum")))
    streams = after["streams"] - before["streams"]
    if streams:
        print("receiver: the demodulator's gain %.1f a symbol level (the hotspot's M17 at COM22: ~1100)"
              % ((after["gain_sum"] - before["gain_sum"]) / 256 / streams))
    tap_path = OUT / "tx_check.dibits"
    tap_path.write_bytes(tap)
    got_path = OUT / "tx_check.rx.c2"
    res = subprocess.run([str(OUT / "m17_core_test.exe"), str(tap_path), str(got_path)], capture_output=True, text=True)
    print("tap decoded: " + res.stdout.strip().replace("\n", "; "))
    if args.source != "sweep":
        return
    pcm_path = OUT / "tx_check.sweep.raw"
    pcm_path.write_bytes(struct.pack("<%dh" % (320 * frames), *sweep(320 * frames)))
    exp_path = OUT / "tx_check.pc.c2"
    subprocess.run([str(OUT / "c2enc_run.exe"), str(pcm_path), str(exp_path)], check=True, capture_output=True)
    exp, got = exp_path.read_bytes(), got_path.read_bytes()
    exp_f = [exp[i:i + 8] for i in range(0, len(exp), 8)]
    got_f = [got[i:i + 8] for i in range(0, len(got), 8)]
    # the tap holds the last stream frames: the received ones end with the transmission's last (partly silence)
    usable = got_f[:-2]  # the last stream frame: silence filled at the release
    best = (-1, None)
    for off in range(0, len(exp_f) - len(usable) + 1):
        same = sum(1 for a, b in zip(usable, exp_f[off:off + len(usable)]) if a == b)
        if same > best[0]:
            best = (same, off)
    print("Codec 2 frames in the tap: %d (the last stream frame left out), identical to the PC encoder's: %d (at "
          "frame %s of %d)" % (len(usable), best[0], best[1], len(exp_f)))


if __name__ == "__main__":
    main()
