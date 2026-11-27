"""The illustrated YSF write-up for the ai repo (OpenDM32/notes/ysf.html): diagrams as inline SVG, charts from the
measurements (matplotlib, PNG inlined), one self-contained file.

  python ysf/py/notes_html.py OUT.html STYLE.css

STYLE.css: the <style> block of the other notes (sms.html), so they look alike.
"""
import base64
import io
import math
import os
import random
import sys
import wave

import numpy as np
import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, '..', 'data')
sys.path.insert(0, HERE)
import demod  # noqa: E402

plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False
BLUE, ORANGE, GREEN, RED, GREY = '#2f6db5', '#c7861a', '#2f8a4a', '#c8382f', '#8a93a6'


def png(fig):
    b = io.BytesIO()
    fig.savefig(b, format='png', dpi=110, bbox_inches='tight', facecolor='white')
    plt.close(fig)
    return ('<img alt="" src="data:image/png;base64,%s" style="max-width:100%%;height:auto;border:1px solid '
            'var(--line);border-radius:6px">' % base64.b64encode(b.getvalue()).decode())


# ---- charts from the measurements ----

def levels_chart():
    """the symbols at the sync fit's gain and DC: our transmitter, the hotspot (-123 dBm), the FTM-400D recording"""
    fig, axes = plt.subplots(1, 3, figsize=(11, 3.0), sharey=False)
    for ax, (name, title, col) in zip(axes, [
            ('cap/own1.raw', '我们发射（COM23→COM22，强信号）', BLUE),
            ('cap/ftm_l128.raw', '热点重放 FTM 帧（-123 dBm）', ORANGE),
            ('rec/dn_py2dza.raw', '八重洲 FTM-400D 录音', GREEN)]):
        x = demod.read(os.path.join(DATA, name))
        x = x - x.mean()
        y = np.convolve(x, demod.rrc(), mode='same')
        vals, t = [], 2.0
        while t < len(y) - 480 * demod.SPS - 10 and len(vals) < 60:
            g, dc, err = demod.fit(y, t, 1)
            if g > 0 and err < 0.6:
                best = (err, t, g, dc)
                for s in np.arange(-1, 1.0001, 1 / 12):
                    g2, dc2, e2 = demod.fit(y, t + s, 1)
                    if g2 > 0 and e2 < best[0]:
                        best = (e2, t + s, g2, dc2)
                vals.append(demod.symbols(y, best[1], best[2], best[3], 1))
                t = best[1] + 470 * demod.SPS
            else:
                t += 1 / 3
        v = np.concatenate(vals)
        ax.hist(v, bins=np.arange(-5, 5.01, 0.125), color=col)
        for lv in (-3, -1, 1, 3):
            ax.axvline(lv, color=GREY, lw=0.8, ls='--')
        ax.set_title(title, fontsize=10)
        ax.set_xlabel('符号电平（按同步字拟合）')
        ax.set_yticks([])
    return png(fig)


def spectrogram_chart():
    w = wave.open(os.path.join(DATA, 'rec', 'dn_decoded.wav'))
    x = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(float)
    fs = w.getframerate()
    x = x[:fs * 8]
    fig, ax = plt.subplots(figsize=(11, 2.6))
    ax.specgram(x, NFFT=256, Fs=fs, noverlap=192, cmap='magma', vmin=-40)
    ax.set_ylim(0, 4000)
    ax.set_xlabel('秒')
    ax.set_ylabel('Hz')
    ax.set_title('FTM-400D 录音：YSF 解帧 → 49 比特 → 72 比特 → 我们的 AMBE+2 解码（前 8 秒）', fontsize=10)
    return png(fig)


# (row, from ms, to ms, label inside the bar); rows 3 / 2 before (TX / RX), 1 / 0 after
EV = [(3, 4, 36, ''), (3, 36, 126, '攒 5 个语音 + 编码'),
      (2, 205, 216, ''), (2, 216, 300, '等整帧收齐（100 ms）'), (2, 300, 312, ''),
      (1, 5, 37, ''), (1, 37, 127, '攒 5 个语音 + 编码'),
      (0, 125, 136, ''), (0, 222, 237, ''), (0, 237, 285, '等 40 ms')]
NOTES = [(3, 4, '射频建立 30 ms'), (3, 126, '第一帧进 FIFO'), (1, 127, '第一帧进 FIFO'), (2, 205, '帧头收齐，开功放'), (2, 300, '解码 +10'), (0, 125, '帧头 FICH，开功放'), (0, 222, '第一段语音')]


def latency_chart():
    fig, ax = plt.subplots(figsize=(11, 3.6))
    for label, y in (('改前 发射', 3), ('改前 接收', 2), ('改后 发射', 1), ('改后 接收', 0)):
        ax.text(-6, y, label, ha='right', va='center', fontsize=10)
    for y, a, b, text in EV:
        col = GREY if y in (1, 3) else (RED if y == 2 else GREEN)
        ax.barh(y, b - a, left=a, height=0.46, color=col, alpha=0.9)
        if text:
            ax.text((a + b) / 2, y, text, ha='center', va='center', fontsize=8.5, color='white')
    for y, x, text in NOTES:
        ax.annotate(text, (x, y + 0.23), (x, y + 0.48), fontsize=8.5, ha='left', va='bottom',
                    arrowprops=dict(arrowstyle='-', color=GREY, lw=0.8))
    ax.axvline(312, color=RED, ls='--', lw=1)
    ax.axvline(285, color=GREEN, ls='--', lw=1)
    ax.text(314, 3.55, '改前出声 312 ms', color=RED, fontsize=9)
    ax.text(287, -0.62, '改后出声 285 ms', color=GREEN, fontsize=9)
    ax.set_xlim(-5, 380)
    ax.set_ylim(-0.9, 3.9)
    ax.set_yticks([])
    ax.set_xlabel('从按 PTT 起的毫秒（两台电台的延迟日志，时钟经写频线对齐）')
    return png(fig)


def margin_chart():
    """the playback's worst margin (simulated): segments' arrival, 32 ms halves, one segment then start_ms"""
    def run(n, start_ms, phase, frames=60, proc=3):
        arr = []
        for k in range(frames):
            for j in range(5):
                t = 100 * k + (192 + 72 * j) / 4.8
                arr.append(math.ceil((t - phase) / 32) * 32 + phase + proc)
        t0 = max(arr[n - 1], arr[0] + start_ms)
        return min(t0 + 20 * i - a for i, a in enumerate(arr))

    rng = random.Random(1)
    sms = list(range(0, 61, 5))
    fig, ax = plt.subplots(figsize=(11, 2.8))
    for n, col in ((1, BLUE), (2, ORANGE), (3, GREEN)):
        ax.plot(sms, [min(run(n, s, rng.uniform(0, 32)) for _ in range(200)) for s in sms], 'o-', color=col,
                label='第 %d 段到后' % n)
    ax.axhline(0, color=RED, lw=1)
    ax.axvline(40, color=GREY, ls='--', lw=1)
    ax.text(41, -25, '选 40 ms', fontsize=9)
    ax.set_xlabel('再等多少毫秒开始放（start_ms）')
    ax.set_ylabel('最坏余量 ms（<0 断音）')
    ax.legend(fontsize=9)
    return png(fig)


# ---- diagrams (SVG) ----

SVG_FRAME = '''<svg viewBox="0 0 900 230" role="img" aria-label="YSF 帧结构">
<rect x="20" y="30" width="38" height="44" class="hdr"/><text x="39" y="57" class="tw" text-anchor="middle">同步</text>
<rect x="58" y="30" width="188" height="44" class="csbk"/><text x="152" y="57" class="tw" text-anchor="middle">FICH 200 比特</text>
<rect x="246" y="30" width="634" height="44" class="blk"/><text x="563" y="57" class="tw" text-anchor="middle">负载 720 比特 = 5 × 144</text>
<text x="20" y="22" class="tm">一帧 100 ms = 960 比特 = 480 符号（4800 符号/秒）</text>
<text x="39" y="92" class="tv" text-anchor="middle">40</text>
<text x="20" y="108" class="tm">D471C9634D</text>
<path d="M246 76 L60 130 M880 76 L880 130" class="dash"/>
<g transform="translate(60,130)">
<rect x="0" y="0" width="42" height="40" class="hdr"/><text x="21" y="24" class="tws" text-anchor="middle">DCH</text>
<rect x="42" y="0" width="122" height="40" class="lst"/><text x="103" y="24" class="tws" text-anchor="middle">语音 104</text>
<rect x="164" y="0" width="42" height="40" class="hdr"/><rect x="206" y="0" width="122" height="40" class="lst"/>
<rect x="328" y="0" width="42" height="40" class="hdr"/><rect x="370" y="0" width="122" height="40" class="lst"/>
<rect x="492" y="0" width="42" height="40" class="hdr"/><rect x="534" y="0" width="122" height="40" class="lst"/>
<rect x="656" y="0" width="42" height="40" class="hdr"/><rect x="698" y="0" width="122" height="40" class="lst"/>
<text x="0" y="58" class="tm">DN（V/D 模式 2）：每段 DCH 40 比特 + 语音 104 比特，一帧 5 段 = 5 个 20 ms 的 AMBE+2</text>
<text x="0" y="78" class="tm">DCH 按帧号 FN 轮流带：0 目标（群呼 10 个 *）、1 呼号、2 下行、3 上行、4 Rem1+2、5 Rem3+电台 ID、6 DT1</text>
</g></svg>'''

SVG_FICH = '''<svg viewBox="0 0 900 150" role="img" aria-label="FICH 编码链">
<g class="t">
<rect x="10" y="40" width="120" height="50" rx="6" class="box"/><text x="70" y="62" text-anchor="middle" class="t">32 比特信息</text><text x="70" y="80" text-anchor="middle" class="tm">FI FN FT DT DG…</text>
<rect x="160" y="40" width="120" height="50" rx="6" class="box"/><text x="220" y="62" text-anchor="middle" class="t">+ CRC-16</text><text x="220" y="80" text-anchor="middle" class="tm">0x1021 取反 → 48</text>
<rect x="310" y="40" width="130" height="50" rx="6" class="box"/><text x="375" y="62" text-anchor="middle" class="t">4 × Golay(24,12)</text><text x="375" y="80" text-anchor="middle" class="tm">各纠 3 错 → 96</text>
<rect x="470" y="40" width="130" height="50" rx="6" class="box"/><text x="535" y="62" text-anchor="middle" class="t">卷积码 1/2 K=5</text><text x="535" y="80" text-anchor="middle" class="tm">+4 尾比特 → 200</text>
<rect x="630" y="40" width="120" height="50" rx="6" class="box"/><text x="690" y="62" text-anchor="middle" class="t">20×5 交织</text><text x="690" y="80" text-anchor="middle" class="tm">按行写按列读</text>
<rect x="780" y="40" width="100" height="50" rx="6" class="box"/><text x="830" y="70" text-anchor="middle" class="t">上空中</text>
</g>
<path d="M130 65 H156 M280 65 H306 M440 65 H466 M600 65 H626 M750 65 H776" class="ln"/>
<text x="10" y="120" class="tm">DCH 相同思路：先白化（PN9 x⁹+x⁵+1），CRC 算在白化后的字节上，再卷积、交织，切 5 段散在帧里。卷积码和 M17 同一对多项式。</text>
</svg>'''

SVG_VOICE = '''<svg viewBox="0 0 900 170" role="img" aria-label="DN 语音的保护">
<text x="10" y="20" class="tm">AMBE+2 半速率一帧 49 比特（和 DMR 帧里的 49 比特相同）</text>
<rect x="10" y="30" width="270" height="34" class="hdr"/><text x="145" y="52" class="tw" text-anchor="middle">前 27 比特（基频、增益、主要谱）</text>
<rect x="280" y="30" width="220" height="34" class="csbk"/><text x="390" y="52" class="tw" text-anchor="middle">后 22 比特</text>
<path d="M145 66 V84 M390 66 V84" class="ln"/>
<rect x="10" y="86" width="270" height="34" class="hdr" opacity="0.9"/><rect x="10" y="86" width="90" height="34" class="hdr"/><text x="145" y="108" class="tw" text-anchor="middle">每比特发 3 遍 → 81，收时三取二</text>
<rect x="280" y="86" width="220" height="34" class="csbk"/><text x="390" y="108" class="tw" text-anchor="middle">发 1 遍 → 22</text>
<rect x="500" y="86" width="16" height="34" class="lst"/><text x="508" y="140" class="tm" text-anchor="middle">0</text>
<path d="M520 103 H556" class="ln"/>
<rect x="560" y="86" width="150" height="34" rx="6" class="box"/><text x="635" y="108" class="t" text-anchor="middle">白化 + 26×4 交织</text>
<path d="M710 103 H746" class="ln"/>
<rect x="750" y="86" width="130" height="34" class="lst"/><text x="815" y="108" class="tw" text-anchor="middle">104 比特</text>
<text x="10" y="160" class="tm">没有 Golay：误码率 = 三元组里不一致的个数（MMDVMHost 的 BER）。49 比特能和 DMR 的 72 比特帧无损互转（静音帧验证）。</text>
</svg>'''

SVG_PIPE = '''<svg viewBox="0 0 900 250" role="img" aria-label="固件里的收发">
<text x="10" y="22" class="t">接收</text>
<rect x="10" y="32" width="140" height="52" rx="6" class="box"/><text x="80" y="54" class="t" text-anchor="middle">鉴频器录音</text><text x="80" y="72" class="tm" text-anchor="middle">8 kHz，32 ms 一块</text>
<rect x="175" y="32" width="160" height="52" rx="6" class="box"/><text x="255" y="54" class="t" text-anchor="middle">p25_demod.c</text><text x="255" y="72" class="tm" text-anchor="middle">C4FM，同步字可配置</text>
<rect x="360" y="32" width="170" height="52" rx="6" class="box"/><text x="445" y="54" class="t" text-anchor="middle">ysf_core.c</text><text x="445" y="72" class="tm" text-anchor="middle">FICH / 每段语音 / DCH</text>
<rect x="555" y="32" width="160" height="52" rx="6" class="box"/><text x="635" y="54" class="t" text-anchor="middle">ysf_radio.c</text><text x="635" y="72" class="tm" text-anchor="middle">通话、呼号、DG-ID</text>
<rect x="740" y="32" width="140" height="52" rx="6" class="box"/><text x="810" y="54" class="t" text-anchor="middle">AMBE+2 解码</text><text x="810" y="72" class="tm" text-anchor="middle">voice_play → 扬声器</text>
<path d="M150 58 H171 M335 58 H356 M530 58 H551 M715 58 H736" class="lns"/>
<text x="10" y="132" class="t">发射</text>
<rect x="10" y="142" width="140" height="52" rx="6" class="box"/><text x="80" y="164" class="t" text-anchor="middle">麦克风</text><text x="80" y="182" class="tm" text-anchor="middle">20 ms 一帧</text>
<rect x="175" y="142" width="160" height="52" rx="6" class="box"/><text x="255" y="164" class="t" text-anchor="middle">AMBE+2 编码</text><text x="255" y="182" class="tm" text-anchor="middle">5 帧凑一个 CC</text>
<rect x="360" y="142" width="170" height="52" rx="6" class="box"/><text x="445" y="164" class="t" text-anchor="middle">ysf_core.c 组帧</text><text x="445" y="182" class="tm" text-anchor="middle">前导 40 ms、HC、CC、TC</text>
<rect x="555" y="142" width="160" height="52" rx="6" class="box"/><text x="635" y="164" class="t" text-anchor="middle">P25 物理层 FIFO</text><text x="635" y="182" class="tm" text-anchor="middle">4800 符号/秒</text>
<rect x="740" y="142" width="140" height="52" rx="6" class="box"/><text x="810" y="164" class="t" text-anchor="middle">发射 ±2.7 kHz</text><text x="810" y="182" class="tm" text-anchor="middle">ysf_tx_dev 1536</text>
<path d="M150 168 H171 M335 168 H356 M530 168 H551 M715 168 H736" class="lng"/>
<text x="10" y="232" class="tm">状态放在 AMBE overlay 后面（15.6 KB），和 NXDN、DMR 软件接收共用这块内存；P25 的解调器只多了一个可配置的同步字（P25 回归逐字节相同）。</text>
</svg>'''


def section(title, body):
    return '<section><h2>%s</h2>%s</section>\n' % (title, body)


def table(head, rows):
    h = ''.join('<th>%s</th>' % c for c in head)
    r = ''.join('<tr>%s</tr>' % ''.join('<td>%s</td>' % c for c in row) for row in rows)
    return '<div class="tbl"><table><tr>%s</tr>%s</table></div>' % (h, r)


def main():
    out, style_path = sys.argv[1], sys.argv[2]
    style = open(style_path, encoding='utf-8').read()
    parts = []
    parts.append('<header><h1>YSF（System Fusion）：原理和 OpenDMR 的实现</h1><p class="lede">2026-11-26 · OpenDMR 分支 '
                 '<code>feature/ysf</code>（17bf37c、aff17f0、8cd12a2）· 依据八重洲公开的《Amateur Radio Digital '
                 'Standards》rev 1.02 自己实现</p></header>')
    parts.append(section('一、YSF 是什么', '''<p>System Fusion 是八重洲 2013 年推出的业余数字模式（电台上叫 C4FM）。和 DMR、P25、NXDN 一样是
<b>四电平 FSK + 数字声码器</b>；不分时隙，一个 12.5 kHz 信道一路通话；用 <b>10 个字符的呼号</b>寻址（像 M17），每帧都写明
数据类型：</p><ul><li><b>DN（V/D 模式 2）</b>：语音 + 数据，AMBE+2 半速率（和 DMR 同一种声码器），最常用——这次做的就是它；</li>
<li>V/D 模式 1：语音带 DMR 那样的 Golay；也能收；</li><li>VW（语音全速率）：IMBE（P25 的声码器），现在只显示呼叫、不放音；</li>
<li>数据全速率：Wires-X、图片。</li></ul><p><b>DG-ID</b>（0 – 99）让热点 / 中继把信号分到不同房间。</p>
<p class="key">调制上它就是“频偏更大的 P25”：4800 符号/秒，dibit 01 → +3、00 → +1、10 → −1、11 → −3，外侧 ±2.7 kHz
（P25 1.8、DMR 1.944），收发都是根升余弦 α = 0.2。所以我们已有的 P25 软件解调器换个同步字就能用。</p>'''))
    parts.append(section('二、帧', '<div class="fig">%s</div><p class="cap">图 1　一帧 100 ms；一次通话：前导（+3 −3）→ 1 个帧头 HC'
                         '（CSD1 = 目标 + 呼号）→ 若干通信帧 CC → 1 个帧尾 TC。</p>' % SVG_FRAME))
    parts.append(section('三、FICH：每帧的“说明书”', '<div class="fig">%s</div><p class="cap">图 2　32 比特的帧信息（帧类型、帧号、'
                         '数据类型、DG-ID…）保护得很重：噪声里也要先认出这是哪种帧。</p>' % SVG_FICH))
    parts.append(section('四、DN 的语音', '<div class="fig">%s</div><p class="cap">图 3　重要的 27 比特发三遍；所以 YSF 和 DMR 的语音帧'
                         '可以直接互转，我们的 AMBE+2 编解码器也直接能用。</p>' % SVG_VOICE))
    parts.append(section('五、固件里的收发', '<div class="fig">%s</div><p class="cap">图 4　和 M17、NXDN 同一套软件接收框架（afrx.c）。'
                         '</p>' % SVG_PIPE))
    parts.append(section('六、做法：先标准、再模型、再真信号、最后固件', '''<ol>
<li>后台查资料：八重洲规范 PDF、MMDVMHost / MMDVM / DSDcc 的事实（不抄代码）、三段公开真实录音 → <code>ysf/facts.md</code>，带测试向量。</li>
<li>Python 模型 <code>ysf/py/ysf.py</code>：规范测试向量全对（FICH 5 组、DCH 2 组、完整帧头、静音帧）。</li>
<li>解真 FTM-400D 录音（降到 8 kHz，和电台录音一样）：203 帧全对、0 误码，语音正常（图 5）。</li>
<li>C 代码 <code>ysf_core.c</code> 在电脑上编译：和 Python 逐字节相同（收的语音帧、发的每个 dibit）。</li>
<li>接入固件、上电台、上热点。</li></ol>''' + spectrogram_chart() + '<p class="cap">图 5　真实八重洲信号经我们的整条接收链（解调、解帧、AMBE+2 解码）。</p>'))
    parts.append(section('七、测试结果', table(['测试', '结果'], [
        ['规范测试向量', '全对'],
        ['FTM-400D 直通录音（22 s）', '203/203 帧，0 误码'],
        ['F5ZOO 中继录音', '326 帧 FICH 全对；呼号 F6FCE、中继 F5ZOO-R1'],
        ['<b>MMDVMHost（热点）收我们发的</b>', '<code>received RF header from GD77 to DG-ID 0</code> … <code>BER: 0.1 – 0.2%</code>：别人的实现确认格式正确'],
        ['热点重放 FTM-400D 原始帧，我们收', '203 帧收到 200，呼号正常（热点信号只有 −123 dBm）'],
        ['弱信号 −114 dBm（COM23 拔天线）', '误符号率 2.2 – 2.9 %；同批次 P25 2.70 %：同等水平'],
        ['中途加入', '0.2 – 0.5 s 出声，约 0.5 s 有呼号'],
        ['发射频偏（以热点 P25 ±1.8 kHz 为尺）', '外侧 ±2.58 kHz（标准 2.7）'],
    ]) + levels_chart() + '<p class="cap">图 6　按同步字拟合后的符号电平分布。我们的信号四个电平很清楚；热点那条是弱信号（同位置热点'
        'P25 也一样散），不是 YSF 解调的问题。</p>'))
    parts.append(section('八、语音延迟：315 → 280 ms', table(['', 'PTT → 对方出声', '断音'], [
        ['第一版', '312 – 328 ms', '每 ~0.8 s 一次'], ['优化后', '<b>274 – 285 ms</b>', '无'],
        ['对照', 'M17 / DMR ~225，P25 ~228，NXDN ~295', '']]) + latency_chart() +
        '''<p class="cap">图 7　两台电台的延迟日志（make DELAY_LOG=1）。灰：发射端；红 / 绿：接收端。</p><ul>
<li><b>边收边解</b>：FICH 一到就开始 / 结束通话，每个语音段一到就解码，不等 100 ms 的整帧；第一段早 60 ms。接收任务每块耗时 30 → 6 ms。</li>
<li><b>起播</b>：原来整帧到了只等 10 ms，下一帧晚到（录音按 32 ms 一块）就断音。模拟各段到达时刻选“第一段后 40 ms”（图 8）。</li>
<li><b>前导</b> 50 → 40 ms。试过 20 ms：第一个语音帧赶不上（物理层提前 30 ms 取块），FIFO 欠载、帧整体错位、开头丢 3 帧。
发射端的真正瓶颈是要先攒够 5 个 20 ms 语音帧。</li></ul>''' + margin_chart() +
        '<p class="cap">图 8　起播余量的模拟（帧内各段相隔 15 ms、帧间 40 ms、32 ms 一块读）。</p>'))
    parts.append(section('九、坑', '''<div class="warn"><ul>
<li><b>截图会饿死接收</b>：fshot.py 读帧缓冲时接收任务漏读 186 块、通话断 8 次，画面在 VFO 和通话之间跳，像界面 bug。界面要用眼睛看。</li>
<li><b>热点自己的 YSF</b>：YSFTXLevel 50 只有 ±2.0 kHz；测试发射器 --level 173 时调制坏掉（录音里一个同步都没有）。先录下来量，才没冤枉接收。</li>
<li>热点同时开 DMR 时，从空闲切到 YSF 的那次常漏帧头：前导 40 ms 3 次里 2 次收到、50 ms 0 次——MMDVM 切模式本身，和我们无关。</li>
<li>VFO 从 DMR（色码 1）切过来曾发 DG-ID 1，热点 DG-ID 网关会当成房间号：DG-ID 改成带标记才算，默认 0。</li>
<li>COM23 有一次刷机重启后 1 ms 定时器中断从开机就停了（旋钮、关机失灵，键盘正常）：拔电池恢复，软重启 8 次没复现。每次刷机后检查 systime_ms。</li>
<li>刷机命令的输出接了一个写错的 grep，管道断了、刷机中途停住，电台停在 updater 里：再跑一次 fw_update.py 就接上了。刷机输出以后只写文件。</li></ul></div>'''))
    parts.append(section('十、还没做', '''<ul><li>VW（IMBE）放音：IMBE 解码器在 P25 的 overlay 里，不能和 AMBE 同时在内存。</li>
<li>CPS 支持 YSF 信道（模式 0xA7、DG-ID）；最近通话记录 YSF；GPS（DT1/DT2）、Wires-X。</li>
<li>和真正的八重洲电台空中互通（手头没有）。</li><li>推送分支、开 PR：等用户确认。</li></ul>'''))
    html = ('<!doctype html>\n<html lang="zh">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, '
            'initial-scale=1">\n<title>YSF System Fusion</title>\n<link rel="stylesheet" href="https://fonts.googleapis.com/css2?'
            'family=Noto+Sans+SC:wght@400;500;700&family=JetBrains+Mono:wght@400;600&display=swap">\n%s\n</head>\n<body>\n<main>\n'
            '%s</main>\n</body>\n</html>\n' % (style, ''.join(parts)))
    open(out, 'w', encoding='utf-8', newline='\n').write(html)
    print('wrote %s (%d KB)' % (out, len(html) // 1024))


if __name__ == '__main__':
    main()
