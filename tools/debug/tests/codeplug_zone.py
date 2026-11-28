"""Helper for the tests here: swaps the radio's zones and channels for one test zone, and puts them back.

The test zone is "Test zone", its channels "Test N" (FM, receive only, 146.520 MHz + (N - 1) x 20 kHz). Sectors are
written as the CPS writes them and read back, then the radio restarts to load them. Needs an English radio with Dual
Watch off and no power-on PIN. Layout: src/services/codeplug.c.

    zone = TestZone(radio, 'zone-saved.bin')   # the radio's sectors saved to that file
    zone.write(1, 2, 3, 4)
    zone.restore()

After a test that stopped half way:  py tools/debug/tests/codeplug_zone.py PORT restore zone-saved.bin
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from radio import Radio  # noqa: E402

SECTOR = 4096
CHUNK = 1024                # 'X' 2: within CABLE_LINK_BUFFER_SIZE (cable_link.h)
ZONE_IN_USE = 0x8010
ZONE_LIST = 0x8030          # the test zone's name here marks a radio holding it: written first, put back last
ZONE_SIZE = 16 + (2 * 80)   # 80 channels a zone (check_channels_per_zone(): the high byte of the 24th channel, 0)
CHANNEL_SIZE = 56
LAST_USED = 0x6000          # "LUCZ", then a byte a zone (the position in it)
SETTINGS = 0x604B           # settings.c: bitfield_options at 36 (bit 10: Dual Watch)
PIN_BLOCK = 0x6100          # security.c: "PIN5", flags (0x01: PIN at power on), set
MAGIC = b'OPENDMR-TEST-ZONE-SAVED-1\n'


def bank_header(bank):
    return 0x3780 if bank == 0 else 0x20000 + 0x7B1B0 + ((bank - 1) * (16 + (128 * CHANNEL_SIZE)))


def channel_address(number):  # 1 - 128
    return 0x3790 + ((number - 1) * CHANNEL_SIZE)


MARKED = ZONE_LIST // SECTOR
SECTORS = [MARKED] + sorted({channel_address(30) // SECTOR} |
                            {a // SECTOR for b in range(8) for a in (bank_header(b), bank_header(b) + 15)} - {MARKED})


def name16(text):
    return text.encode('ascii') + (b'\xff' * (16 - len(text)))


def test_tables(saved, numbers):
    """The saved sectors with only the test zone and its channels in use"""
    tables = {s: bytearray(d) for s, d in saved.items()}

    def put(address, data):
        for i, octet in enumerate(data):
            tables[(address + i) // SECTOR][(address + i) % SECTOR] = octet

    put(ZONE_IN_USE, b'\x01' + bytes(31))
    zone = name16('Test zone') + b''.join(struct.pack('<H', n) for n in numbers)
    put(ZONE_LIST, zone + bytes(ZONE_SIZE - len(zone)))
    in_use = bytearray(16)
    for n in numbers:
        in_use[(n - 1) // 8] |= 1 << ((n - 1) % 8)
        bcd = int('%d' % (14652000 + ((n - 1) * 2000)), 16)  # 10 Hz units, BCD
        record = bytearray(CHANNEL_SIZE)
        record[0:16] = name16('Test %d' % n)
        record[16:24] = struct.pack('<II', bcd, bcd)  # rx_freq, tx_freq
        record[32:36] = b'\xff' * 4                    # rx_tone, tx_tone: none
        record[51] = 0x04                              # flag4: receive only
        record[55] = 10                                # sql
        put(channel_address(n), record)
    for bank in range(8):
        put(bank_header(bank), in_use if bank == 0 else bytes(16))
    return tables


class TestZone:
    def __init__(self, radio, path, saved=None):
        self.radio, self.port, self.path, self.saved = radio, radio.port, path, saved
        self.zone, self.home = None, None
        if saved is not None:
            return
        if os.path.exists(path):
            sys.exit('%s is there from a test that stopped: put the radio\'s tables back first: py %s PORT restore %s'
                     % (path, os.path.abspath(__file__), path))
        s = radio.home()
        if s['screen'] == 'dual_watch' or (struct.unpack('<I', self.read(SETTINGS + 36, 4))[0] & (1 << 10)):
            sys.exit('Dual Watch is on (it takes the keys): turn it off and save, then run the test again')
        pin = self.read(PIN_BLOCK, 6)
        if (pin[:4] == b'PIN5') and (pin[5] == 1) and (pin[4] & 0x01):
            sys.exit('a PIN is asked at power on: the restart would lock the cable until it is typed. Turn it off')
        self.home = s['home']
        self.zone = int(channel_screen(radio).get('zone', '-1'))
        self.saved = {s: self.read_sector(s) for s in SECTORS}
        if self.holds_test_zone(self.saved[MARKED]):
            sys.exit('the radio holds a test zone already: put its own tables back first (restore, with the file '
                     'saved by the test that wrote it)')
        with open(path, 'wb') as f:
            f.write(MAGIC + b''.join(struct.pack('>I', s) + self.saved[s] for s in SECTORS))
        print('%s: the radio\'s zone and channel tables saved (%d sectors)' % (path, len(SECTORS)))

    @classmethod
    def from_file(cls, radio, path):
        with open(path, 'rb') as f:
            blob = f.read()
        body, size = blob[len(MAGIC):], 4 + SECTOR
        saved = {struct.unpack('>I', body[i:i + 4])[0]: body[i + 4:i + size] for i in range(0, len(body), size)}
        if not blob.startswith(MAGIC) or (len(body) != len(SECTORS) * size) or (sorted(saved) != sorted(SECTORS)):
            sys.exit('%s was not saved by codeplug_zone.py' % path)
        return cls(radio, path, saved)

    @staticmethod
    def holds_test_zone(sector):
        return sector[ZONE_LIST % SECTOR:][:9] == b'Test zone'

    # ---- the cable

    def request(self, data, header, timeout=3.0, retries=3):
        for _ in range(retries):
            self.port.reset_input_buffer()
            self.port.write(data)
            got, end = b'', time.time() + timeout
            while (time.time() < end) and (header not in got) and (b'-' not in got):
                got += self.port.read(max(1, self.port.in_waiting))
            if header in got:
                return
            time.sleep(0.2)
        raise RuntimeError('no answer to %r' % data[:2])

    def read(self, address, length):
        """'D' 'Z' (debug.c): packed, a run of 3 or more as 0x80 + n - 3 and the octet, else c < 0x80 and c + 1 octets"""
        out = bytearray()
        while len(out) < length:
            n = min(512, length - len(out))
            for _ in range(3):
                self.port.reset_input_buffer()
                self.port.write(b'DZ' + struct.pack('>IH', address + len(out), n))
                got, end, i = b'', time.time() + 2.0, -1
                while time.time() < end:
                    got += self.port.read(max(1, self.port.in_waiting))
                    i = got.find(b'DZ')
                    if (i >= 0) and (len(got) >= i + 5) and (len(got) >= i + 5 + ((got[i + 3] << 8) | got[i + 4])):
                        break
                if (i >= 0) and (len(got) >= i + 5) and (got[i + 2] == 1):
                    packed, unpacked, j = got[i + 5:i + 5 + ((got[i + 3] << 8) | got[i + 4])], bytearray(), 0
                    while j < len(packed):
                        c = packed[j]
                        unpacked += packed[j + 1:j + 2 + c] if c < 0x80 else (packed[j + 1:j + 2] * (c - 0x80 + 3))
                        j += (c + 2) if c < 0x80 else 2
                    if len(unpacked) == n:
                        out += unpacked
                        break
                time.sleep(0.2)
            else:
                raise RuntimeError('no answer to D Z at 0x%06X' % (address + len(out)))
        return bytes(out)

    def read_sector(self, s):
        first = self.read(s * SECTOR, SECTOR)
        if self.read(s * SECTOR, SECTOR) != first:
            sys.exit('sector 0x%X read differently twice: check the cable (nothing written)' % s)
        return first

    def abort(self):
        """A sector prepared and not written: dropped (an 'X' 2 longer than the buffer is refused, which drops it).
        Until then 'D' 'Z' has no buffer (cable_link_sector_buffer())"""
        try:
            self.request(b'X\x02' + bytes(4) + b'\xff\xff', b'-')
        except RuntimeError:
            pass

    def write_sector(self, s, data):
        for _ in range(3):
            try:
                self.request(b'X\x01' + s.to_bytes(3, 'big'), b'X\x01')
                for at in range(0, SECTOR, CHUNK):
                    self.request(b'X\x02' + struct.pack('>IH', (s * SECTOR) + at, CHUNK) + data[at:at + CHUNK],
                                 b'X\x02')
                self.request(b'X\x03', b'X\x03', timeout=5.0)
            except RuntimeError:
                self.abort()
            if self.read(s * SECTOR, SECTOR) == data:
                return
        raise RuntimeError('sector 0x%X not written: switch the radio off and on, then: py %s PORT restore %s'
                           % (s, os.path.abspath(__file__), self.path))

    def put(self, tables, order):
        for s in order:
            if self.read(s * SECTOR, SECTOR) != bytes(tables[s]):
                self.write_sector(s, bytes(tables[s]))
        self.restart()

    def restart(self):
        """'C' 6 0, as the CPS after a write: the radio restarts and reads the codeplug again (no power-off save)"""
        self.request(b'C\x06\x00', b'-')  # its answer, then the restart 0.5 s later
        time.sleep(3)
        end = time.time() + 30
        while True:
            try:
                return self.radio.wait(lambda s: s['screen'] in ('channel', 'vfo'), 2.0)
            except Exception:
                if time.time() > end:
                    raise RuntimeError('the radio did not come back after its restart')
                time.sleep(0.5)

    # ---- the test

    def write(self, *numbers):
        assert all(1 <= n <= 30 for n in numbers)
        self.put(test_tables(self.saved, numbers), SECTORS)  # the marked sector first

    def restore(self):
        self.abort()
        if not self.holds_test_zone(self.read(MARKED * SECTOR, SECTOR)):
            print('no test zone on the radio: nothing put back (its tables are its own, or put back already)')
        else:
            self.put(self.saved, SECTORS[1:] + [MARKED])  # the marked sector last
            print('the radio\'s zone and channel tables put back')
            if self.zone is not None:
                self.back_on_zone()
        os.replace(self.path, self.path + '.restored')

    def back_on_zone(self):
        """The zone and the screen (channel / VFO) the radio was on when the test began"""
        try:
            channel_screen(self.radio)
            s = self.radio.main_menu('Zone')
            for _ in range(int(s.get('count', '0') or 0)):
                if s.get('sel') == str(self.zone):
                    self.radio.key('green')
                    break
                self.radio.key('down', settle=0.05)
                s = self.radio.state()
            channel_screen(self.radio)
            if self.home == 'vfo':
                self.radio.key('red')
            self.radio.home()
        except Exception as e:  # the tables are back: only the zone shown is not
            print('the radio could not be put back on its zone %d (%s)' % (self.zone, e))

    def in_use(self, number):
        """Channel number (1 - 128) in use in the flash"""
        return ((self.read(bank_header(0), 16)[(number - 1) // 8] >> ((number - 1) % 8)) & 1) == 1

    def saved_position(self, zone_index):
        """The position in that zone saved at the last power off (0 when none saved)"""
        table = self.read(LAST_USED, 4 + 70)
        return table[4 + zone_index] if table[:4] == b'LUCZ' else 0


# ---- the keys of the tests (an English radio: rows by their names)

def channel_screen(radio):
    """The channel screen (the right key on the VFO)"""
    if radio.home()['screen'] == 'vfo':
        radio.key('red')
    return radio.wait(lambda s: s['screen'] == 'channel')


def pick_zone(radio, name):
    """The zone list (main menu, Zone), that zone picked, the channel screen"""
    channel_screen(radio)
    radio.main_menu('Zone')
    radio.row(name)
    radio.key('green')
    return radio.wait(lambda s: s['screen'] == 'channel')


def go_to(radio, name, most=40):
    """The channel screen moved on (up key) to the channel of that name"""
    s = radio.state()
    for _ in range(most):
        if s.get('name') == name:
            return s
        radio.key('up')
        s = radio.state()
    raise RuntimeError('no channel %r in this zone' % name)


def new_channel(radio):
    """The VFO, then its quick menu, VFO --> New Chan (the first press asks to confirm), back on the channel screen"""
    channel_screen(radio)
    radio.key('red')
    radio.wait(lambda s: s['screen'] == 'vfo')
    radio.quick_menu('VFO --> New Chan')
    radio.key('green')
    radio.key('green')
    return radio.wait(lambda s: s['screen'] == 'channel')


if __name__ == '__main__':
    if (len(sys.argv) != 4) or (sys.argv[2] != 'restore') or not os.path.exists(sys.argv[3]):
        sys.exit(__doc__)
    TestZone.from_file(Radio(sys.argv[1]), sys.argv[3]).restore()
