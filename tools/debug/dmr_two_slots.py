#!/usr/bin/env python3
"""A DMR frame file for the hotspot's test transmitter (pistar-signal-gen p25tx: DMR1 / DMR2 / SHORTLC lines, the
two slots interleaved burst by burst) with a call on each timeslot at once: one slot the 1031 Hz tone call of
files/1031Hz_test_dmr.txt, the other the same call with every AMBE+2 frame the standard silence frame (the sync, the
EMB, the slot type, the LCs left as they are). A radio listening to one slot that plays the other's frames shows it in
its RX_CAPTURE frames (tools/debug/rx_capture.py): tone frames where silence was sent, or the reverse.

  py tools/debug/dmr_two_slots.py [--tone-slot 1] [--seconds 20] [--src E:/exam/pistar-signal-gen/files/1031Hz_test_dmr.txt] out.txt
  py tools/debug/dmr_two_slots.py --ts1 0:15 --ts2 5:20 out.txt   (each slot's call from - to, s; a slot idle meanwhile)
"""
import argparse
from pathlib import Path

SILENCE = bytes.fromhex('B9E881526173002A6B')  # AMBE+2 (DMR) silence frame, 72 bits as sent
BURST_S = 0.06                                   # a superframe burst of one slot comes every 60 ms


def bits(data):
    return ''.join(f'{b:08b}' for b in data)


def unbits(s):
    return bytes(int(s[i:i + 8], 2) for i in range(0, len(s), 8))


def silenced(burst):
    """the 3 AMBE frames of a voice burst (bits 0-107 and 156-263 around the sync / EMB) replaced by silence"""
    b = bits(burst)
    s = bits(SILENCE) * 3
    return unbits(s[:108] + b[108:156] + s[108:])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--tone-slot', type=int, choices=(1, 2), default=1)
    ap.add_argument('--seconds', type=float, default=20)
    ap.add_argument('--src', type=Path, default=Path('E:/exam/pistar-signal-gen/files/1031Hz_test_dmr.txt'))
    ap.add_argument('--ts1', help='TS1 call from:to (s); default: all the time; "off": none')
    ap.add_argument('--ts2', help='TS2 call from:to (s); "off": none')
    args = ap.parse_args()
    lines = [l.split() for l in args.src.read_text().splitlines() if l.strip()]
    shortlc = [l for l in lines if l[0] == 'SHORTLC']
    bursts = [bytes.fromhex(l[2]) for l in lines if l[0] in ('DMR1', 'DMR2')]
    header, voice, term = bursts[0], bursts[1:-1], bursts[-1]
    assert len(voice) == 6, 'a header, the voice bursts A - F, a terminator'
    def call(silent, span):
        if span == 'off':
            return [], 0
        start, end = (0.0, args.seconds) if not span else map(float, span.split(':'))
        n = max(1, round((end - start) / (BURST_S * 6)))
        body = [silenced(v) for v in voice] if silent else voice
        lead = round(start / BURST_S)  # bursts of this slot idle first (the modem fills them)
        return [None] * lead + [header] + body * n + [term], n

    slots = {}
    for ts, span in ((1, args.ts1), (2, args.ts2)):
        slots[ts], superframes = call(ts != args.tone_slot, span)
    steps = max(len(slots[1]), len(slots[2]))
    with args.out.open('w', newline='\n') as f:
        for k, n, h in shortlc:
            print(k, n, h, file=f)
        for i in range(steps):
            for ts in (1, 2):
                b = slots[ts][i] if i < len(slots[ts]) else None
                if b is not None:
                    print('DMR%d' % ts, len(b), b.hex().upper(), file=f)
    print('%s: TS1 %s, TS2 %s, the tone on TS%d, silence on TS%d'
          % (args.out, args.ts1 or 'all', args.ts2 or 'all', args.tone_slot, 3 - args.tone_slot))


if __name__ == '__main__':
    main()
