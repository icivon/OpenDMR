"""The hotspot sends an M17 frame file N times; after each, the receiver's counters (did it find the transmission?).

  python m17/py/m17_trials.py FILE [N] [--seconds 8] [--port COM22]
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'nxdn', 'py'))
import pi  # noqa: E402


def counters(port):
    out = subprocess.run([sys.executable, os.path.join(HERE, 'm17_monitor.py'), '--port', port], capture_output=True,
                         text=True).stdout
    return dict((k, int(v)) for k, v in (p.rsplit(' ', 1) for p in out.splitlines()[0].split(', ')))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('file')
    ap.add_argument('n', type=int, nargs='?', default=6)
    ap.add_argument('--seconds', type=int, default=8)
    ap.add_argument('--port', default='COM22')
    a = ap.parse_args()
    c = pi.connect()
    hits = 0
    for i in range(a.n):
        b = counters(a.port)
        pi.run(c, 'setsid nohup /tmp/m17play.sh %s %d > /dev/null 2>&1 & echo' % (a.file, a.seconds))
        time.sleep(a.seconds + 8)
        e = counters(a.port)
        d = {k: e[k] - b[k] for k in ('streams', 'bad_frames', 'lsfs', 'eots', 'calls', 'underruns')}
        hits += d['streams'] > 0
        print(i, d, flush=True)
    print('found %d of %d' % (hits, a.n))


if __name__ == '__main__':
    main()
