"""Radio set up for YSF tests over the cable (tools/debug/radio.py): the VFO in YSF mode, simplex on a frequency.

  python ysf/py/rig.py COM22 [433.5]     VFO, YSF, Rx = Tx = the frequency; prints the state
"""
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'debug'))
from radio import Radio  # noqa: E402


def to_vfo(r):
    s = r.home()
    if s['screen'] != 'vfo':
        r.key('red')  # the channel screen's back key: the VFO
        s = r.wait(lambda s: s['screen'] == 'vfo')
    return s


def to_mode(r, mode='YSF'):
    s = r.state()
    for _ in range(12):
        if s['mode'] == mode:
            return s
        r.key('star', mod='sk1')
        time.sleep(0.4)
        s = r.state()
    raise RuntimeError('mode %s not reached (%s)' % (mode, s['mode']))


def simplex(r, mhz):
    """Rx and Tx both on mhz: Rx typed (tune()), then SK2+down selects Tx, the digits, SK2+up back to Rx"""
    units = int(round(mhz * 100000))
    s = r.tune(mhz)
    if s.get('tx') != str(units):
        r.key('down', mod='sk2')
        time.sleep(0.2)
        r.keys(*('%08d' % units), settle=0.15)
        time.sleep(0.3)
        r.key('up', mod='sk2')
        time.sleep(0.2)
        s = r.state()
    if s.get('rx') != str(units) or s.get('tx') != str(units):
        raise RuntimeError('not simplex on %s: %s' % (mhz, s))
    return s


def setup(port, mhz):
    r = Radio(port)
    to_vfo(r)
    to_mode(r, 'YSF')
    s = simplex(r, mhz)
    r.close()
    return s


if __name__ == '__main__':
    print(setup(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 433.5))
