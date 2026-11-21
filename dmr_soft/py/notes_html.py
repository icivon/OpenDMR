"""The software DMR receive write-up (HTML, Chinese) with its figures worked out from the recordings in
dmr_soft/data (dmrtone_*.raw: the tone test transmission at about -116 dBm).

  python dmr_soft/py/notes_html.py out.html
"""
import html
import string
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dmr_lab as L  # noqa: E402
import dmr_tone as T  # noqa: E402
import dmr_tune as U  # noqa: E402

DATA = HERE.parent / 'data'
W = 760


# ---- small SVG helpers ----

def svg(w, h, body, label):
    return ('<svg viewBox="0 0 %d %d" role="img" aria-label="%s" xmlns="http://www.w3.org/2000/svg">%s</svg>'
            % (w, h, html.escape(label), body))


def text(x, y, s, cls='t', anchor='start'):
    return '<text x="%.1f" y="%.1f" class="%s" text-anchor="%s">%s</text>' % (x, y, cls, anchor, html.escape(s))


def line(x1, y1, x2, y2, cls='ax'):
    return '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" class="%s"/>' % (x1, y1, x2, y2, cls)


def poly(pts, cls):
    return '<polyline class="%s" points="%s"/>' % (cls, ' '.join('%.1f,%.1f' % p for p in pts))


def rect(x, y, w, h, cls):
    return '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" class="%s"/>' % (x, y, w, h, cls)


def circle(x, y, r, cls):
    return '<circle cx="%.1f" cy="%.1f" r="%.1f" class="%s"/>' % (x, y, r, cls)


class Axes:
    def __init__(self, x0, y0, w, h, xr, yr):
        self.x0, self.y0, self.w, self.h, self.xr, self.yr = x0, y0, w, h, xr, yr

    def X(self, v):
        return self.x0 + (v - self.xr[0]) / (self.xr[1] - self.xr[0]) * self.w

    def Y(self, v):
        return self.y0 + self.h - (v - self.yr[0]) / (self.yr[1] - self.yr[0]) * self.h

    def frame(self):
        return line(self.x0, self.y0 + self.h, self.x0 + self.w, self.y0 + self.h) + line(self.x0, self.y0, self.x0,
                                                                                          self.y0 + self.h)


# ---- the data ----

def strong_burst():
    """a burst of the tone recordings: the filtered samples, its timing, gain and DC"""
    x = np.fromfile(DATA / 'dmrtone_3.raw', '<i2').astype(float)
    x -= x.mean()
    xf = np.convolve(x, T.rrc(0.2), mode='same')
    t0, name, pol, g, dc, e = [s for s in L.find_syncs(xf, 0.6) if s[1] == 'MS voice'][1]
    return x, xf, t0, g, dc


def fig_samples():
    x, xf, t0, g, dc = strong_burst()
    s0 = t0 + 54 * L.SPS - 6 * L.SPS          # some symbols before the sync's
    n_sym = 22
    span = n_sym * L.SPS
    a = Axes(46, 18, W - 70, 220, (0, span), (-5.2, 5.2))
    body = a.frame()
    for lv in (-3, -1, 1, 3):
        body += line(a.X(0), a.Y(lv), a.X(span), a.Y(lv), 'lvl') + text(a.x0 - 8, a.Y(lv) + 4, '%+d' % lv, 'tm', 'end')
    for lv in (-2, 0, 2):
        body += line(a.X(0), a.Y(lv), a.X(span), a.Y(lv), 'thr')
    ts = np.linspace(s0, s0 + span, 400)
    curve = (L.interp(xf, ts) - dc) / g
    body += poly([(a.X(t - s0), a.Y(v)) for t, v in zip(ts, curve)], 'curve')
    i0 = int(np.ceil(s0))
    for i in range(i0, int(s0 + span)):
        body += circle(a.X(i - s0), a.Y((xf[i] - dc) / g), 3.2, 'samp')
    for k in range(n_sym):
        t = s0 + k * L.SPS
        v = (L.interp(xf, np.array([t]))[0] - dc) / g
        body += line(a.X(t - s0), a.Y(-5.2), a.X(t - s0), a.Y(5.2), 'symt')
        body += rect(a.X(t - s0) - 4, a.Y(v) - 4, 8, 8, 'symv')
    body += text(a.x0 + 4, a.y0 + a.h + 18, '圆点：8 kHz 的采样（RRC 之后）；方块：符号时刻的插值；竖线：符号时刻，每 5/3 个采样一个', 'tm')
    return svg(W, 270, body, '采样点与符号时刻')


def fig_rrc():
    h = T.rrc(0.2)
    n = len(h)
    c = n // 2
    a = Axes(46, 14, W - 70, 170, (-c - 1, c + 1), (-0.15, 0.68))
    body = a.frame() + line(a.X(-c - 1), a.Y(0), a.X(c + 1), a.Y(0), 'lvl')
    for i, v in enumerate(h):
        x = a.X(i - c)
        body += rect(x - 7, min(a.Y(v), a.Y(0)), 14, abs(a.Y(v) - a.Y(0)), 'bar')
    for k in range(-6, 7):
        if k:
            body += line(a.X(k * 5 / 3), a.y0, a.X(k * 5 / 3), a.y0 + a.h, 'symt')
    body += text(a.X(0), a.y0 + a.h + 18, '0', 'tm', 'middle')
    body += text(a.X(5 / 3), a.y0 + a.h + 18, '1 个符号', 'tm', 'middle')
    body += text(a.X(-c), a.y0 + 12, '21 个系数（Q12 后和为 4096）；虚线：每个符号的间隔', 'tm')
    return svg(W, 210, body, 'RRC 冲激响应')


def fig_sync_fit():
    x, xf, t0, g, dc = strong_burst()
    pat = L.symbols_of([k for k, n in L.SYNCS.items() if n == 'MS voice'][0], 24)
    ts = t0 + (54 + np.arange(24)) * L.SPS
    y = L.interp(xf, ts)
    gg, dd, e = L.fit(y, pat)
    a = Axes(46, 14, W - 70, 200, (-0.5, 23.5), (-4.5, 4.5))
    body = a.frame()
    for lv in (-3, -1, 1, 3):
        body += line(a.X(-0.5), a.Y(lv), a.X(23.5), a.Y(lv), 'lvl') + text(a.x0 - 8, a.Y(lv) + 4, '%+d' % lv, 'tm', 'end')
    for k in range(24):
        v = (y[k] - dd) / gg
        body += line(a.X(k - 0.4), a.Y(pat[k]), a.X(k + 0.4), a.Y(pat[k]), 'fitlv')
        body += line(a.X(k), a.Y(pat[k]), a.X(k), a.Y(v), 'resid')
        body += circle(a.X(k), a.Y(v), 4, 'samp')
    body += text(a.x0 + 4, a.y0 + a.h + 18,
                 'MS 语音同步的 24 个符号：横线 = 已知电平，圆点 = (采样 − dc) / g，红线 = 残差；误差 = 残差平方的平均 = %.2f'
                 % e, 'tm')
    return svg(W, 245, body, '同步字最小二乘拟合')


def all_bursts():
    return U.bursts([DATA / ('dmrtone_%d.raw' % i) for i in range(6)], True)


def fig_hist(zs):
    Z = np.concatenate(zs)
    d = np.clip(np.round((Z + 3) / 2) * 2 - 3, -3, 3)
    m = {lv: Z[d == lv].mean() for lv in (-3, -1, 1, 3)}
    dd = [(m[-3] + m[-1]) / 2, (m[-1] + m[1]) / 2, (m[1] + m[3]) / 2]
    hist, edges = np.histogram(Z, bins=120, range=(-5, 5))
    a = Axes(46, 14, W - 70, 200, (-5, 5), (0, hist.max() * 1.1))
    body = a.frame()
    for i, c in enumerate(hist):
        body += rect(a.X(edges[i]), a.Y(c), a.X(edges[i + 1]) - a.X(edges[i]) - 0.6, a.Y(0) - a.Y(c), 'hbar')
    for v in (-2, 0, 2):
        body += line(a.X(v), a.y0, a.X(v), a.y0 + a.h, 'thr')
    for v in dd:
        body += line(a.X(v), a.y0, a.X(v), a.y0 + a.h, 'ddthr')
    for lv in (-3, -1, 1, 3):
        body += text(a.X(m[lv]), a.y0 + 12, '%.2f' % m[lv], 'tm', 'middle')
        body += text(a.X(lv), a.y0 + a.h + 16, '%+d' % lv, 'tm', 'middle')
    body += text(a.x0 + 4, a.y0 + a.h + 34, '灰虚线：固定门限 0、±2；绿线：按四个簇的中心取中点（%s）' % ', '.join('%.2f' % v for v in dd), 'tm')
    return svg(W, 262, body, '符号值分布'), m, dd


def isi_points(zs, eq):
    X, Y = [], []
    for z in zs:
        y = U.relevel(z, 'dd', 0)
        if eq:
            y2 = y.copy()
            y2[1:-1] = (243 * y[1:-1] - 13 * (y[:-2] + y[2:])) / 256
            y = y2
        d = np.clip(np.round((y + 3) / 2) * 2 - 3, -3, 3)
        for k in range(1, 131):
            X.append(d[k - 1] + d[k + 1])
            Y.append(y[k] - d[k])
    X, Y = np.array(X), np.array(Y)
    return {v: Y[X == v].mean() for v in range(-6, 7, 2) if np.any(X == v)}


def fig_isi(zs):
    before, after = isi_points(zs, False), isi_points(zs, True)
    a = Axes(60, 14, W - 90, 200, (-7, 7), (-0.55, 0.55))
    body = a.frame() + line(a.X(-7), a.Y(0), a.X(7), a.Y(0), 'lvl')
    for v in range(-6, 7, 2):
        body += text(a.X(v), a.y0 + a.h + 16, '%+d' % v, 'tm', 'middle')
        if v in before:
            body += rect(a.X(v) - 15, min(a.Y(before[v]), a.Y(0)), 14, abs(a.Y(before[v]) - a.Y(0)), 'bar2')
        if v in after:
            body += rect(a.X(v) + 1, min(a.Y(after[v]), a.Y(0)), 14, abs(a.Y(after[v]) - a.Y(0)), 'bar')
    for v in (-0.4, -0.2, 0.2, 0.4):
        body += text(a.x0 - 8, a.Y(v) + 4, '%+.1f' % v, 'tm', 'end')
    body += text(a.x0 + 4, a.y0 + a.h + 34,
                 '横轴：前后两个邻居符号之和；纵轴：本符号偏离自己电平的平均值。橙：均衡前，蓝：均衡后', 'tm')
    return svg(W, 262, body, '码间干扰'), before, after


def fig_bars(groups, ymax, label, unit='%'):
    """groups: [(name, [(label, value, cls)])]"""
    n = sum(len(g[1]) for g in groups) + len(groups)
    a = Axes(60, 14, W - 90, 190, (0, n), (0, ymax))
    body = a.frame()
    x = 0.5
    for name, bars in groups:
        x0 = x
        for lab, v, cls in bars:
            body += rect(a.X(x), a.Y(v), a.X(x + 0.8) - a.X(x), a.Y(0) - a.Y(v), cls)
            body += text(a.X(x + 0.4), a.Y(v) - 5, ('%.1f' % v) + unit, 'tv', 'middle')
            body += text(a.X(x + 0.4), a.y0 + a.h + 15, lab, 'tm', 'middle')
            x += 1
        body += text((a.X(x0) + a.X(x - 0.2)) / 2, a.y0 + a.h + 33, name, 't', 'middle')
        x += 1
    return svg(W, 255, body, label)


PIPELINE = '''
<svg viewBox="0 0 760 150" role="img" aria-label="接收链" xmlns="http://www.w3.org/2000/svg">
<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" class="arrow"/></marker></defs>
<g class="blk">
<rect x="4" y="40" width="100" height="56" rx="8"/><rect x="128" y="40" width="88" height="56" rx="8"/>
<rect x="240" y="40" width="112" height="56" rx="8"/><rect x="376" y="40" width="112" height="56" rx="8"/>
<rect x="512" y="40" width="112" height="56" rx="8"/><rect x="648" y="40" width="108" height="56" rx="8"/>
</g>
<g class="arr"><line x1="104" y1="68" x2="126" y2="68" marker-end="url(#ar)"/><line x1="216" y1="68" x2="238" y2="68" marker-end="url(#ar)"/>
<line x1="352" y1="68" x2="374" y2="68" marker-end="url(#ar)"/><line x1="488" y1="68" x2="510" y2="68" marker-end="url(#ar)"/>
<line x1="624" y1="68" x2="646" y2="68" marker-end="url(#ar)"/></g>
<text x="54" y="64" class="t" text-anchor="middle">鉴频器录音</text><text x="54" y="84" class="tm" text-anchor="middle">8 kHz</text>
<text x="172" y="64" class="t" text-anchor="middle">RRC</text><text x="172" y="84" class="tm" text-anchor="middle">匹配滤波</text>
<text x="296" y="64" class="t" text-anchor="middle">同步 + 时间轴</text><text x="296" y="84" class="tm" text-anchor="middle">定时 / 增益 / 直流</text>
<text x="432" y="64" class="t" text-anchor="middle">判决电平</text><text x="432" y="84" class="tm" text-anchor="middle">每突发四个中心</text>
<text x="568" y="64" class="t" text-anchor="middle">3 抽头均衡</text><text x="568" y="84" class="tm" text-anchor="middle">去码间干扰</text>
<text x="702" y="64" class="t" text-anchor="middle">判决 → 二层</text><text x="702" y="84" class="tm" text-anchor="middle">FEC / 声码器</text>
<text x="380" y="128" class="tm" text-anchor="middle">前三步决定"每个符号读得准不准"，后两步决定"门限放在哪"——弱信号下两者都要做</text>
</svg>'''

TIMELINE = '''
<svg viewBox="0 0 760 170" role="img" aria-label="时间轴" xmlns="http://www.w3.org/2000/svg">
<g>
''' + ''.join('<rect x="%d" y="40" width="%d" height="34" rx="3" class="%s"/><text x="%d" y="62" class="tm" text-anchor="middle">%s</text>'
              % (14 + 58 * i, 52, 'slotA' if i % 2 == 0 else 'slotB', 40 + 58 * i,
                 ('ABCDEF'[i // 2] if i % 2 == 0 else '·') if i < 12 else ('A' if i % 2 == 0 else '·'))
              for i in range(13)) + '''
</g>
<g class="arr"><line x1="40" y1="96" x2="40" y2="80"/><line x1="736" y1="96" x2="736" y2="80"/></g>
<text x="40" y="112" class="tm" text-anchor="middle">语音同步</text><text x="746" y="112" class="tm" text-anchor="end">下一个语音同步（360 ms 后）</text>
<text x="14" y="28" class="tm">每 30 ms 一个突发（240 个采样 = 144 个符号）；本时隙每 60 ms 一个；另一个时隙（·）直通时为空</text>
<text x="14" y="140" class="tm">中间的 B–F 没有同步字：时间轴从同步处一直外推；每个突发都在预期位置 ±1 个采样内再找一次同步，</text>
<text x="14" y="158" class="tm">找到就重新定时；偏离时间轴的强同步（另一次发射）立即换到新的时间轴</text>
</svg>'''

MASK = '''
<svg viewBox="0 0 760 150" role="img" aria-label="符号位掩码" xmlns="http://www.w3.org/2000/svg">
''' + ''.join('<rect x="%d" y="30" width="15" height="22" class="%s"/>' % (14 + 18 * i, 'bit1' if (i * 3 + 1) % 5 < 3 else 'bit0') for i in range(40)) \
    + ''.join('<rect x="%d" y="70" width="15" height="22" class="%s"/>' % (14 + 18 * ((5 * k + 1) // 3), 'mask') for k in range(24)) + '''
<text x="14" y="22" class="tm">每个采样入环时记 1 位：在长期均值之上为 1（深色）</text>
<text x="14" y="110" class="tm">某个相位下 24 个符号各取最近的采样（框）：一个 64 位掩码。同步字在这些位置上的期望符号也是 64 位常数，</text>
<text x="14" y="128" class="tm">一次 XOR + popcount 得到"符号一致的个数"，≥21 才去做最小二乘拟合。原先逐个插值 + 64 位除法，每 32 ms 要算 113 ms</text>
</svg>'''


def main():
    out = Path(sys.argv[1])
    zs = all_bursts()
    hist_svg, means, ddthr = fig_hist(zs)
    isi_svg, before, after = fig_isi(zs)
    progression = fig_bars([
        ('离线（纠错后全对，第 2、3 帧，−116 dBm）', [('芯片', 67.0, 'barc'), ('原始', 29.3, 'bar2'), ('+RRC', 33.0, 'bar2'),
                                          ('+判决电平', 45.3, 'bar2'), ('+均衡', 88.4, 'bar')]),
    ], 100, '逐步改进')
    blocks = fig_bars([
        ('芯片', [('C0', 2.37, 'barc'), ('C1', 6.28, 'barc'), ('C2', 3.45, 'barc'), ('C3', 3.95, 'barc')]),
        ('软件 RRC + 判决电平', [('C0', 0.19, 'bar2'), ('C1', 3.00, 'bar2'), ('C2', 2.82, 'bar2'), ('C3', 2.82, 'bar2')]),
        ('+ 均衡', [('C0', 0.18, 'bar'), ('C1', 0.91, 'bar'), ('C2', 0.46, 'bar'), ('C3', 0.55, 'bar')]),
    ], 7, '各块原始误码率')
    live = fig_bars([
        ('直通 −116 dBm', [('芯片', 0.3, 'barc'), ('软件', 0.0, 'bar')]),
        ('热点双时隙 −122 dBm', [('芯片', 14.3, 'barc'), ('软件', 2.6, 'bar')]),
    ], 16, '实机坏帧率')
    doc = string.Template(TEMPLATE).substitute(
        pipeline=PIPELINE, samples=fig_samples(), rrc=fig_rrc(), syncfit=fig_sync_fit(), timeline=TIMELINE,
        hist=hist_svg, isi=isi_svg, progression=progression, blocks=blocks, live=live, mask=MASK,
        m3='%.2f' % means[-3], m1='%.2f' % means[-1], p1='%.2f' % means[1], p3='%.2f' % means[3], t0='%.2f' % ddthr[1],
        b6='%.2f' % before[6], a6='%.2f' % after[6], bm6='%.2f' % before[-6], am6='%.2f' % after[-6])
    out.write_text(doc, encoding='utf-8', newline='\n')
    print('wrote', out, len(doc), 'bytes')


TEMPLATE = Path(HERE / 'notes_template.html').read_text(encoding='utf-8') if (HERE / 'notes_template.html').exists() else ''

if __name__ == '__main__':
    main()
