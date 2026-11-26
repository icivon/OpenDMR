#!/usr/bin/env python3
"""Diagnose why the DM-36 side buttons SK1 / SK2 do nothing.

Commit 297b1d5 measured only the matrix keys (menu, back, *, 0, up, down).
SK1 / SK2 were never measured: the checklist in docs/dm36-porting.md still has
"SK1 / SK2 real GPIO (DM32 = 51 / 52)" unchecked. Meanwhile the stock firmware
has no separate side-button read at all: its only key detector (0x30124c4)
drives the keypad rows low first (0x3011cc8), then samples the columns. So on
the DM-36 the side buttons may be matrix positions or different GPIOs, not
necessarily the DM-32's direct-to-column pins 51 / 52.

This tool tells the three cases apart in one pass, while a button is held:

  1) firmware keys_read_buttons() reports SK1 / SK2  -> GPIO layer OK, the
     fault is upstream (input.c / keymap.c);
  2) raw GPIO levels change but the function does not -> wrong pin number or
     the stock "rows driven low" precondition is missing;
  3) nothing changes -> the button needs rows driven low (matrix position).

Usage (radio on, connected, DM32_PORT overrides COM6, DM32_ELF overrides the ELF):

    py tools/debug/dm36_keyscan.py          # hold SK1
    py tools/debug/dm36_keyscan.py sk2      # hold SK2
    py tools/debug/dm36_keyscan.py ptt      # hold PTT
"""
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dm32dbg  # noqa: E402

ELF = os.environ.get('DM32_ELF', os.path.join(HERE, '..', '..', 'src', 'build', 'opendmr_dm36.elf'))
# GPIO_EXT(pin) = GPIO_BASE(pin) + 0x50; A = 0..31, B = 32..63, C = 64..95
BANKS = [('A', 0x14020050, 0), ('B', 0x14100050, 32), ('C', 0x14110050, 64)]
KEYS_BUTTON = {'SK1': 0x01, 'SK2': 0x02, 'PTT': 0x04}


def symbols(elf):
    out = {}
    for line in subprocess.run([dm32dbg.NM, elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            out[p[2]] = int(p[0], 16)
    return out


def raw_buttons(radio, addr):
    return radio.call(addr, []) & 0xFF


def snap_gpio(radio):
    """Read both words of every GPIO_EXT twice; keep the stable level."""
    out = {}
    for name, addr, base in BANKS:
        a = struct.unpack('<I', radio.mem_read(addr, 4))[0]
        time.sleep(0.01)
        b = struct.unpack('<I', radio.mem_read(addr, 4))[0]
        out[name] = (a & b, base)
    return out


def main():
    target = (sys.argv[1] if len(sys.argv) > 1 else 'sk1').lower()
    sym = symbols(ELF)
    addr_buttons = sym.get('keys_read_buttons')
    addr_keypad = sym.get('keys_read_keypad')
    radio = dm32dbg.Radio(os.environ.get('DM32_PORT', 'COM6'))

    print(f'DM-36 key diagnosis -- hold: {target}')
    print('Release all buttons, then press Enter ...')
    input()
    idle_btn = raw_buttons(radio, addr_buttons) if addr_buttons else 0
    idle_gpio = snap_gpio(radio)

    print(f'Now HOLD {target}, then press Enter ...')
    input()
    held_btn = raw_buttons(radio, addr_buttons) if addr_buttons else 0
    held_gpio = snap_gpio(radio)

    print('\n=== firmware input layer ===')
    if addr_buttons:
        print(f'  keys_read_buttons(): 0x{idle_btn:02X} -> 0x{held_btn:02X}')
        for name, bit in KEYS_BUTTON.items():
            if (held_btn & bit) and not (idle_btn & bit):
                print(f'    {name} bit SET -> GPIO + input task see the button')
    else:
        print('  keys_read_buttons() symbol not found in', ELF)

    if addr_keypad:
        kp = radio.call(addr_keypad, [])
        print(f'  keys_read_keypad(): 0x{kp:X} (matrix bits)')

    print('\n=== raw GPIO_EXT pins that changed (pin: idle -> held) ===')
    changed = []
    for name, _, _ in BANKS:
        a, base = idle_gpio[name]
        b, _ = held_gpio[name]
        for i in range(32):
            if ((a >> i) & 1) != ((b >> i) & 1):
                pin = base + i
                changed.append(pin)
                print(f'  pin {pin:>2}: {"HIGH" if (a >> i) & 1 else "LOW "}'
                      f' -> {"HIGH" if (b >> i) & 1 else "LOW "}')
    if not changed:
        print('  (none) -> the button needs the rows driven low to register.')

    print('\n=== verdict ===')
    held_sk = held_btn & (KEYS_BUTTON['SK1'] | KEYS_BUTTON['SK2'])
    if held_sk:
        print('  GPIO/input layer OK. Check input.c / keymap.c and the UI bindings.')
    elif changed:
        print('  GPIO level changes but keys_read_buttons() misses it: '
              'set BOARD_PIN_KEY_3/4 to the pins above in board_dm36.h.')
    else:
        print('  No level change with rows idle: needs the stock row-drive '
              'precondition; capture the active (row,col) and add it to '
              'BOARD_KEY_INDEX in board_dm36.h.')


if __name__ == '__main__':
    main()
