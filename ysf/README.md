# YSF（Yaesu System Fusion）

OpenDMR 的 YSF 收发（分支 `feature/ysf`）。依据八重洲公开的《Amateur Radio Digital Standards》rev 1.02 自己实现；
MMDVMHost、MMDVM、DSDcc 等开源实现只读了事实（数值、表格、帧序），没有用代码。整理出的空中接口见 [facts.md](facts.md)。

## 目录

| 路径 | 内容 |
|---|---|
| `facts.md` | 空中接口：调制、帧、FICH、DCH、V/D 模式 1/2、VW、测试向量、各来源的出处和不确定的地方 |
| `py/ysf.py` | Python 模型：CRC、Golay(24,12)、卷积码和 Viterbi、白化、交织、FICH / DCH / V/D 2 语音的编解码、HC / CC / TC 帧。`python ysf/py/ysf.py` 用规范的测试向量自测 |
| `py/demod.py` | 鉴频器录音（任意采样率的 wav、DSDcc 的 .dis、电台的 8 kHz .raw）→ 8 kHz → 找同步 → 每帧按同步拟合增益和直流 → 解帧，语音写成 DMR 72 比特 AMBE 帧（`--bit`） |
| `c/ysf_test.c`、`c/build.sh` | 电脑上编译固件的 `ysf_core.c` + `p25_demod.c`：`rx`（录音）、`dibits`、`tx`（生成一段 DN 通话） |
| `py/rig.py` | 电台设成 VFO、YSF、单工某频率 |
| `py/tx_check.py` | 电台对电台：一台按 PTT 若干秒，另一台读接收计数和最后的帧记录（`ysf_frame_log`） |
| `py/capture.py`、`py/record.py` | 接收电台切到 M17 模式，用 M17 的原始录音 tap 录 1.5 秒鉴频器输出（自己发 / 热点发） |
| `py/levels.py`、`py/ser.py` | 录音的电平分布、频偏；固件解调器的误符号率（解出来的帧重新编码对比） |
| `py/hs_rx.py` | 热点（pistar-signal-gen，加了 YSF）发帧文件，电台收，语音帧和原始帧逐个比对 |
| `data/` | 规范 PDF、录音、抓取，**不进仓库** |

## 原理（简）

- C4FM：4800 符号/秒四电平 FSK，dibit 01 → +3、00 → +1、10 → −1、11 → −3（同 DMR、P25），宽带 ±900 / ±2700 Hz，
  根升余弦 α = 0.2。
- 一帧 100 ms = 960 比特：同步 `D471C9634D`（40）+ FICH（200）+ 负载（720）。
- FICH：32 比特信息（FI 帧类型、FN/FT 帧号、DT 数据类型、SQ/DG-ID ……）+ CRC-16 → 4 个 Golay(24,12) → 4 个尾比特 →
  码率 1/2 K=5 卷积码（和 M17 同一对多项式）→ 20×5 交织。
- DCH：数据先白化（PN9），CRC 算在白化后的字节上，再卷积、交织，切成 5 段散在帧里。
- DN（V/D 模式 2）：每帧 5 × (DCH 40 比特 + 语音 104 比特)。语音是 AMBE+2 半速率的 49 比特：前 27 比特各发三遍、
  后 22 比特发一遍、补一个 0，白化后 26×4 交织。没有 Golay，靠三取二。49 比特和 DMR 的 72 比特帧可以互转（静音帧验证）。
- 通话：前导（+3 −3）→ 1 个帧头 HC（CSD1：目标 + 呼号）→ CC 帧（FN 0..6 依次带目标、呼号、上下行、Rem、DT1）→ 帧尾 TC。

## 固件里怎么做的

- 接收：`afrx.c` 录下鉴频器（8 kHz，不去加重）→ `p25_demod.c`（加了可配置的同步字，P25 默认不变，回归测试 6 段录音逐字节相同；
  YSF 用 20 符号同步、20 个符号的正负号至少 17 个对）→ `ysf_core.c`（找同步、锁定后按 480 dibit 一帧走、FICH 坏了用上一帧
  推测、4 帧都不行算丢失）→ `ysf_radio.c`（通话、呼号、DG-ID，语音 49 比特 → 72 比特 → 我们的 AMBE+2 解码 → `voice_play.c`）。
  状态在 AMBE overlay 后面（`ckcpu.ld .ysf_rx`，15.6 KB），和 NXDN / DMR 软件接收共用。
- 发射：麦克风 → AMBE+2 编码，5 帧凑一个 CC；前导 240 dibit + HC 先进 FIFO 再按发射；松 PTT 时补静音、发 TC。
  P25 物理层 FIFO（4800 符号/秒），调制电平 `ysf_tx_dev` 1536（1024 = P25 的 ±1.8 kHz → ±2.7 kHz）。
- DG-ID：存在信道的色码字节里，`unused2` 带 0xB0 标记才算（和 P25 NAC 一样的办法），否则 0——VFO 从 DMR 色码 1 切过来
  曾经发 DG-ID 1，热点的 DG-ID 网关会把它当成一个房间。发射带上；接收不过滤（同 M17 的 CAN），显示对方的。
- 界面：信道 / VFO 画面频率下面显示 `全部` 和 `DG-ID n`；通话时大字呼号（`qso_panel_draw_ysf`，和 M17 共用一个绘制函数）。

## 测试结果（2026-11-26）

| 测试 | 结果 |
|---|---|
| Python 模型 vs 规范测试向量 | FICH 5 组、DCH 2 组、整帧 HC、静音帧全对 |
| 真实 FTM-400D 直通录音（DN，22 s） | 203/203 帧，0 误码；语音用我们的 AMBE+2 解码正常（`data/rec/dn_decoded.wav`） |
| F5ZOO 中继录音 | 326 帧 FICH 全对，呼号 F6FCE、中继 F5ZOO-R1 |
| C（固件代码）vs Python | 语音帧逐字节相同；发射输出逐 dibit 相同；收发回环 0 误码 |
| 电台对电台（强信号） | 32 帧 0 错 |
| **MMDVMHost 收我们的发射** | `YSF, received RF header from GD77 to DG-ID 0` … `end of transmission, 4.2 seconds, BER: 0.2%`（另一次 0.1 %） |
| 热点重放 FTM-400D 原始帧（−123 dBm） | 203 帧收到 200，呼号正常；重复码误码 6.9 %（信号太弱） |
| 弱信号（COM23 去天线，−114 dBm） | 误符号率 2.2 – 2.9 %；同一批次 P25 2.70 % —— 和 P25 同等水平 |
| 中途加入 | 切到 YSF 后 0.2 – 0.5 s 开始放音，约 0.5 s 显示呼号 |
| 我们的发射频偏 | 用热点 P25（±1.8 kHz）做标尺：外侧 ±2.58 kHz（标准 2.7，−4.5 %） |

## 语音延迟（2026-11-26）

`make DELAY_LOG=1`（只开 64 条的延迟日志；整个 DEBUG 版已经放不下内存），`tools/debug/voice_delay.py --tx COM23 --rx COM22`。

| | PTT → 对方出声 |
|---|---|
| 第一版 | 312 – 328 ms，而且每 ~0.8 s 断一次音 |
| 边收边解 + 前导 40 ms + 起播 1 段后 40 ms | **274 – 285 ms**，不断音 |

- 时间线（改后）：35 ms 开始发射（射频建立 30 ms，各模式一样）→ 127 ms 第一个语音帧编好进 FIFO（攒 5 个 20 ms）→
  接收端 125 ms 收到帧头的 FICH 就开功放 → 222 ms 第一个语音帧的 FICH → 第一段语音约 15 ms 后 → 等 40 ms → 出声。
- 接收：`ysf_core.c` 不再等整帧（100 ms）才解，FICH 一到（第 120 个 dibit）、每个语音段一到（第 192 + 72 j 个）就解，
  最后一段和 DCH 在帧尾。接收任务每个半缓冲最长耗时 30 → 6 ms。
- 起播：原来整帧 5 段到了再等 10 ms，下一帧晚到超过 10 ms 就断（录音按 32 ms 一块读）。模拟各段到达时刻（帧内相隔 15 ms，
  帧间 40 ms，32 ms 一块）：第一段到后等 40 ms 留约 12 ms 余量；30 ms 只剩 2 ms。
- 前导：20 ms 时第一个语音帧赶不上（物理层提前 30 ms 取块），FIFO 欠载，后面的帧整体错位、开头丢 3 帧；40 ms 正常。
  热点（DMR + YSF 都开）从空闲切到 YSF 的那次常漏帧头（late entry），前导 40 ms 3 次里 2 次收到帧头，50 ms 0 次：
  是 MMDVM 多模式切换本身，和前导长短无关。

## 踩过的坑

- **刷机命令不能接过滤**：`fw_update.py ... | grep` 的模式写错时管道断开，刷机中途停住，电台停在 updater 里（看起来没反应）；
  不要关机，直接再跑一次 `fw_update.py`（它会接上）。以后输出只写文件。
- COM23 有一次刷机重启后 1 ms 定时器中断从开机就停了（旋钮、关机失灵，键盘正常；耗时统计读成 0xFFFFFFFF）。
  软重启 8 次没复现。每次刷机后检查两台的 `systime_ms` 在走。
- 热点的 YSF 发射：MMDVMHost YSFTXLevel=50 时外侧只有 ±2.0 kHz；pistar-signal-gen `--level 173` 时热点自己的调制坏掉
  （录音里一个同步都找不到），不是我们的接收。热点信号到电台只有 −123 dBm，只能做互通，不能比灵敏度。
- **截图会饿死接收**：`fshot.py` 读帧缓冲时接收任务漏读 186 个半缓冲、通话断了 8 次，画面在 VFO 和通话之间来回跳（看起来像
  界面 bug）。不截图时 0 漏读。看界面用眼睛，或只在通话后截。
- 发射结束后的 60 ms 收尾等待里 FIFO 计 2 次欠载（NXDN / M17 一样），不影响帧。

## 还没做

- VW（全速率，IMBE）放音：IMBE 解码器在 P25 overlay 里，和 AMBE overlay 不能同时用；现在只显示呼叫、状态栏 `VW`。
- CPS 支持 YSF 信道（模式标记 0xA7、DG-ID）；最近通话记录 YSF 呼叫；DT1/DT2（GPS）、Wires-X、数据 FR。
- 和真八重洲电台空中互通（手头没有八重洲电台）。
