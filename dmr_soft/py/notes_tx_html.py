"""The software DMR transmit write-up (HTML, Chinese): the timeslot timing and the PA switching, with the measurements
of 2026-11-20 (the tone test frames received in software at about -116 dBm).

  python dmr_soft/py/notes_tx_html.py out.html
"""
import string
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from notes_html import W, Axes, svg, text, line, rect, poly, fig_bars  # noqa: E402

# ---- measured (dmr_soft/README.md, the session's tests) ----

# the HR_C7000 engine, direct mode, UHF: rf_on_waits (us) -> errors in the first 8 bits of each burst (%)
ENGINE_WAIT = [(0, 1.5), (200, 19.7), (400, 4.0), (600, 27.3), (1200, 47.6)]
# software, PA on time after the empty block's interrupt (us) -> errors in the first 8 bits (%)
GATE_ON = [(24700, 0.0), (25700, 1.6), (26000, 3.9), (26500, 2.3), (27000, 0.0), (27500, 12.5), (28100, 48.0)]
# PA off time after the burst's interrupt (us) -> errors in the last 8 bits (%)
GATE_OFF = [(26900, 48.0), (27500, 45.3), (28000, 19.5), (28500, 0.0), (28700, 0.8), (30000, 0.0)]
# the FD6818 tuned this long before the PA (us) -> the first AMBE frame right after FEC (%)
TUNE_LEAD = [(0, 47), (1500, 72), (2500, 77), (4000, 82)]


def fig_slots():
    """two timeslots: the blocks, the burst in ours, the PA window, the interrupts"""
    a = Axes(20, 30, W - 40, 150, (-2, 62), (0, 10))
    body = ''
    # blocks
    for k, (x0, name, cls) in enumerate([(0, '我们的时隙（偶数块）', 'slotA'), (30, '另一个时隙（奇数块，空）', 'slotB')]):
        body += rect(a.X(x0), a.Y(6.5), a.X(x0 + 30) - a.X(x0) - 2, a.Y(3) - a.Y(6.5), cls)
        body += text(a.X(x0 + 15), a.Y(6.9), name, 'tm', 'middle')
    # the burst: symbols 6 .. 137 of 144: 1.25 ms .. 28.75 ms
    body += rect(a.X(1.25), a.Y(6.0), a.X(28.75) - a.X(1.25), a.Y(3.5) - a.Y(6.0), 'burst')
    body += text(a.X(15), a.Y(4.4), '突发 132 个符号（27.5 ms）', 't', 'middle')
    body += text(a.X(0.6), a.Y(2.4), '6', 'tm', 'middle') + text(a.X(29.4), a.Y(2.4), '6', 'tm', 'middle')
    # PA window
    pa = [(a.X(-2), a.Y(1.2)), (a.X(-0.1), a.Y(1.2)), (a.X(0.1), a.Y(2.0)), (a.X(29.0), a.Y(2.0)), (a.X(29.2), a.Y(1.2)),
          (a.X(62), a.Y(1.2))]
    body += poly(pa, 'pa')
    body += text(a.X(32), a.Y(1.6), '功放：只在我们的时隙开', 'tm')
    for x in (0, 30, 60):
        body += line(a.X(x), a.Y(9.5), a.X(x), a.Y(0.5), 'symt')
        body += text(a.X(x), a.Y(9.8), '%d ms' % x, 'tm', 'middle')
    return svg(W, 200, body, '时隙与突发')


def fig_timeline():
    """one cycle on the time axis from the empty block's interrupt"""
    a = Axes(30, 20, W - 60, 230, (20, 60), (0, 10))
    body = a.frame()
    for x in range(20, 61, 5):
        body += line(a.X(x), a.Y(0), a.X(x), a.Y(10), 'symt') + text(a.X(x), a.Y(-0.7), '%d' % x, 'tm', 'middle')
    rows = [
        (9.0, '物理层中断', [(0, 'odd'), (27.4, 'even')]),
    ]
    # the interrupts
    body += text(a.X(20.3), a.Y(9.3), '中断（空块）在 0 ms；中断（突发块）在 ~27.4 ms：两种中断不是均匀的 30 ms', 'tm')
    body += line(a.X(27.4), a.Y(8.6), a.X(27.4), a.Y(7.6), 'irq') + text(a.X(27.4), a.Y(7.2), '中断（突发块）', 'tm', 'middle')
    # the data
    body += rect(a.X(28.2), a.Y(6.3), a.X(55.7) - a.X(28.2), a.Y(5.1) - a.Y(6.3), 'burst')
    body += text(a.X(42), a.Y(5.5), '突发数据 28.2 – 55.7 ms', 't', 'middle')
    # FD6818
    body += rect(a.X(22.9), a.Y(4.4), a.X(28.2) - a.X(22.9), a.Y(3.6) - a.Y(4.4), 'tune')
    body += text(a.X(22.9), a.Y(4.75), 'FD6818 切到发射频率 22.9 ms（提前 4 ms 让它稳定）', 'tm')
    # PA
    pa = [(a.X(20), a.Y(1.0)), (a.X(26.9), a.Y(1.0)), (a.X(27.1), a.Y(2.6)), (a.X(56.1), a.Y(2.6)), (a.X(56.2), a.Y(1.0)),
          (a.X(60), a.Y(1.0))]
    body += poly(pa, 'pa')
    body += text(a.X(20.3), a.Y(2.2), '功放 26.9 ms 起爬坡，27.1 满功率', 'tm')
    body += text(a.X(59.8), a.Y(3.0), '数据结束后 0.4 ms 关（突发块中断后 +28.7）', 'tm', 'end')
    # the mask
    body += rect(a.X(26.7), a.Y(2.9), a.X(28.2) - a.X(26.7), a.Y(0.6) - a.Y(2.9), 'mask')
    body += text(a.X(27.45), a.Y(0.25), '标准允许的爬坡区 1.5 ms', 'tm', 'middle')
    return svg(W, 280, body, '一个周期的时间线')


def fig_curve(points, xr, yr, xlab, ylab, marks=(), cls='curve2'):
    a = Axes(60, 26, W - 90, 180, xr, yr)
    body = a.frame()
    for x, lab in marks:
        body += line(a.X(x), a.y0, a.X(x), a.y0 + a.h, 'ddthr') + text(a.X(x) + 4, a.y0 + 12, lab, 'tm')
    body += poly([(a.X(x), a.Y(y)) for x, y in points], cls)
    for x, y in points:
        body += '<circle cx="%.1f" cy="%.1f" r="4" class="dot"/>' % (a.X(x), a.Y(y))
        body += text(a.X(x), a.Y(y) - 8, '%g' % y, 'tv', 'middle')
    for v in (yr[0], (yr[0] + yr[1]) / 2, yr[1]):
        body += text(a.x0 - 8, a.Y(v) + 4, '%g' % v, 'tm', 'end')
    for x, _ in points:
        body += text(a.X(x), a.y0 + a.h + 15, '%g' % x, 'tm', 'middle')
    body += text(a.x0 + a.w / 2, a.y0 + a.h + 33, xlab, 'tm', 'middle')
    body += text(a.x0 + 8, a.y0 + 12, ylab, 'tm')
    return svg(W, 250, body, ylab)


def main():
    out = Path(sys.argv[1])
    head = (HERE / 'notes_template.html').read_text(encoding='utf-8')
    head = head[:head.index('<body>')].replace('<title>DMR 软件接收</title>', '<title>DMR 软件发射</title>')
    head = head.replace('</style>', '''svg .slotA { fill: var(--soft); stroke: var(--sig); }
svg .burst { fill: var(--sig); opacity: .85; }
svg .tune { fill: var(--sig2); opacity: .8; }
svg .pa { fill: none; stroke: var(--good); stroke-width: 2.5; }
svg .mask { fill: var(--bad); opacity: .15; }
svg .irq { stroke: var(--bad); stroke-width: 2; }
svg .curve2 { fill: none; stroke: var(--sig); stroke-width: 2; }
svg .dot { fill: var(--sig); }
</style>''')
    body = string.Template((HERE / 'notes_tx_body.html').read_text(encoding='utf-8')).substitute(
        slots=fig_slots(), timeline=fig_timeline(),
        engine=fig_curve(ENGINE_WAIT, (-100, 1300), (0, 55), '芯片引擎：中断后等待多久才改频开功放（µs）', '前 8 位误码 %'),
        gate_on=fig_curve(GATE_ON, (24500, 28300), (0, 55), '软件：空块中断后多久开功放（µs）', '前 8 位误码 %',
                          marks=[(26900, '采用 26900')]),
        gate_off=fig_curve(GATE_OFF, (26700, 30200), (0, 55), '软件：突发块中断后多久关功放（µs）', '最后 8 位误码 %',
                           marks=[(28700, '采用 28700')]),
        tune=fig_curve(TUNE_LEAD, (-300, 4300), (0, 100), 'FD6818 比功放提前多久改频（µs）', '第 1 帧纠错后全对 %'),
        result=fig_bars([
            ('第 1 帧坏帧', [('芯片发射', 46.0, 'barc'), ('芯片 + #95', 2.0, 'bar2'), ('软件发射', 0.0, 'bar')]),
            ('第 2、3 帧坏帧', [('芯片发射', 1.0, 'barc'), ('芯片 + #95', 1.5, 'bar2'), ('软件发射', 0.0, 'bar')]),
        ], 50, '发射结果'))
    out.write_text(head + body, encoding='utf-8', newline='\n')
    print('wrote', out, out.stat().st_size, 'bytes')


if __name__ == '__main__':
    main()
