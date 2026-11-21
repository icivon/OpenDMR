"""Codec 2 3200 frames (8 bytes each) into an M17 stream as the hotspot's transmitter plays it (frame text file, one
frame a line: M17LSF / M17STREAM / M17EOT and 48 bytes in hex; the modem sends its own preamble): an LSF (broadcast,
from the source callsign, voice, not encrypted), a stream frame for every two Codec 2 frames, the last with the end
bit, the EoT. From the M17 specification, as src/services/m17_core.c receives it.

  python m17/py/m17_frames.py in.c2 out.txt [--src VA3BPF]
"""
import argparse
from pathlib import Path

RANDOMIZER = bytes.fromhex(
    "d6b5e23082ff8462ba4e9690d898dd5d0cc85243911df86e682f35da14eacd76198dd580d1338713571 82d2978c3".replace(" ", ""))
ALPHABET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/."
GOLAY_ROWS = (0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB)
P1 = (1,) + (1, 0, 1, 1) * 15  # the LSF's puncturing
P2 = (1,) * 11 + (0,)          # a stream frame's


def bits(data):
    return [(b >> (7 - i)) & 1 for b in data for i in range(8)]


def pack(bitlist):
    return bytes(int("".join(map(str, bitlist[i:i + 8])), 2) for i in range(0, len(bitlist), 8))


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x5935 if crc & 0x8000 else 0)) & 0xFFFF
    return crc


def conv(data, puncture):
    """K=5 rate 1/2 (G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4), 4 flush bits, punctured"""
    state, coded = 0, []
    for bit in data + [0] * 4:
        state = ((state << 1) | bit) & 31
        coded.extend(((state & 0x19).bit_count() & 1, (state & 0x17).bit_count() & 1))
    return [bit for i, bit in enumerate(coded) if puncture[i % len(puncture)]]


def frame(sync, payload):
    """the QPP interleaver, the randomizer, the sync burst first"""
    assert len(payload) == 368
    ordered = [0] * 368
    for i, bit in enumerate(payload):
        ordered[(45 * i + 92 * i * i) % 368] = bit
    return sync + bytes(a ^ b for a, b in zip(pack(ordered), RANDOMIZER))


def lich(lsf, index):
    """40 bits of the LSF and the chunk number, as 4 Golay(24, 12) words"""
    chunk = (int.from_bytes(lsf[5 * index:5 * index + 5], "big") << 8) | (index << 5)
    out = b""
    for shift in (36, 24, 12, 0):
        value = (chunk >> shift) & 4095
        check = 0
        for row, bit in zip(GOLAY_ROWS, range(11, -1, -1)):
            if value & (1 << bit):
                check ^= row
        out += ((value << 12) | check).to_bytes(3, "big")
    return bits(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--src", default="VA3BPF")
    args = ap.parse_args()
    assert len(RANDOMIZER) == 46
    c2 = args.input.read_bytes()
    frames = len(c2) // 16
    source = sum(ALPHABET.index(ch) * 40 ** i for i, ch in enumerate(args.src.upper()))
    lsf = b"\xff" * 6 + source.to_bytes(6, "big") + b"\x00\x05" + bytes(14)
    lsf += crc16(lsf).to_bytes(2, "big")
    with args.output.open("w", encoding="ascii", newline="\n") as f:
        def emit(kind, data):
            print(kind, len(data), data.hex(), file=f)
        emit("M17LSF", frame(bytes.fromhex("55f7"), conv(bits(lsf), P1)))
        for n in range(frames):
            last = 0x8000 if n == frames - 1 else 0
            data = ((n & 0x7FFF) | last).to_bytes(2, "big") + c2[16 * n:16 * n + 16]
            emit("M17STREAM", frame(bytes.fromhex("ff5d"), lich(lsf, n % 6) + conv(bits(data), P2)))
        emit("M17EOT", bytes.fromhex("555d") * 24)
    print("%s: %d stream frames, %.1f s" % (args.output, frames, frames * 0.04))


if __name__ == "__main__":
    main()
