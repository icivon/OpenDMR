"""DMR software receive, evaluated per voice superframe: the MS voice syncs split into time line segments (one
transmission each, at least 2 syncs), every burst from the first sync to 5 after the last taken on the segment's fitted
time line, the frames judged as our decoder does (C0 + C1 corrections > 3: bad), by position in the burst (position 1
is damaged by our own transmitter). Four receivers: plain; RRC 0.2; + per-burst decision-directed levels; + the fixed
3-tap equaliser [-0.05, 0.95, -0.05].

  python dmr_soft/py/dmr_eval.py dmr_soft/data/dmrtone_*.raw
"""
import glob
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dmr_frames as F  # noqa: E402
import dmr_lab as L  # noqa: E402
import dmr_tone as T  # noqa: E402
import dmr_tune as U  # noqa: E402

W = np.array([-0.05, 0.95, -0.05])
LV = {3: 1, 1: 0, -1: 2, -3: 3}


def segments(s):
    segs=[[s[0]]]
    for a in s[1:]:
        d=(a[0]-segs[-1][-1][0])/T.BURST
        if abs(d-round(d))<0.03: segs[-1].append(a)
        else: segs.append([a])
    return segs
def run(paths, rrc, dd, eq):
    h=T.rrc(0.2); errs={0:[],1:[],2:[]}
    for p in paths:
        x=np.fromfile(p,'<i2').astype(float); x-=x.mean()
        x0=x
        if rrc: x=np.convolve(x,h,mode='same')
        s=[a for a in L.find_syncs(x0 if not rrc else x,0.6) if a[1]=='MS voice']
        if not s: continue
        for seg in [q for q in segments(s) if len(q)>1]:
            n=np.array([round(a[0]/T.BURST) for a in seg]); t=np.array([a[0] for a in seg])
            if len(seg)>1: b,a0=np.polyfit(n,t,1)
            else: b,a0=T.BURST,t[0]-n[0]*T.BURST
            for k in range(n.min(),n.max()+6):
                tt=a0+b*k
                if tt<4 or tt+133*L.SPS>len(x)-4: continue
                j=np.argmin(abs(n-k)); g,dc=seg[j][3],seg[j][4]
                z=(L.interp(x,tt+np.arange(132)*L.SPS)-dc)/g
                if dd: z=U.relevel(z,'dd',0)
                if eq: y=z.copy(); y[1:-1]=np.convolve(z,W[::-1],mode='valid'); z=y
                fr=F.burst_frames([LV[int(v)] for v in F.decide(z)])
                for f in range(3): errs[f].append(F.frame_errors(fr[f]))
    a=np.array(errs[1]+errs[2]); o=np.array(errs[0])
    return 'pos2-3: %d frames bad %.1f%% (0 err %.0f%%) | pos1: bad %.1f%%'%(len(a),100*np.mean(a>3),100*np.mean(a==0),100*np.mean(o>3))
if __name__ == '__main__':
    ps = sorted(p for g in sys.argv[1:] for p in glob.glob(g))
    for cfg,name in (((0,0,0),'plain'),((1,0,0),'rrc'),((1,1,0),'rrc+dd'),((1,1,1),'rrc+dd+eq')):
        print('%-10s %s' % (name, run(ps, *cfg)))
