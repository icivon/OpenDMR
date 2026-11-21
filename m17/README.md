# M17 收发

固件里的 M17：

- 接收：HR_C7000 以 8 kHz 录下鉴频器输出（和 NXDN 一样），软件解调、帧处理、Codec 2 3200 解码，经提示音 FIFO
  播放。
- 发射：麦克风 20 ms 一帧经 Codec 2 3200 编码，两帧一个流帧（前导码、LSF、流帧、末帧带结束位、EoT），dibit 经
  P25 物理层的发射 FIFO（4800 符号/秒，与 M17 相同）发出，调制器的 4FSK 电平按 M17 的 ±2.4 kHz 缩放
  （`m17_tx_dev`，以 P25 的 ±1.8 kHz 为 1024）。源呼号取写频里的呼号（Radio name / callsign），目的为广播 ALL。

全部是依据 M17 规范和 Codec 2 参考实现的算法自行实现的整数代码（编码器另有每帧几十次 float），没有第三方代码。

| 文件 | 内容 |
|---|---|
| `src/services/m17_demod.c` | 4FSK 软件解调：8 kHz 上按分数时间三次插值取符号，同步字最小二乘拟合时序、增益、直流；输出软符号（电平 ×16） |
| `src/services/m17_core.c` | 帧：同步、去扰码、QPP 解交织、软判决 Viterbi（P1/P2 打孔）、CRC、LICH（Golay 24,12）拼 LSF、呼号；发射方向的卷积编码、打孔、交织、加扰、LICH、LSF |
| `src/services/c2dec.c` | Codec 2 3200 定点解码，算法见 [codec2-3200.md](codec2-3200.md) 第 1–8 节 |
| `src/services/c2enc.c` | Codec 2 3200 定点编码，同上第 9 节 |
| `src/services/m17_radio.c` | 录音 → 解调 → 帧 → 解码 → 播放；按 PTT 时麦克风 → 编码 → 帧 → 发射；状态、编解码器（发射时编码器与解码器、播放队列共用 RAM）、任务栈都在 M17 覆盖区 RAM |

## PC 上的测试（`C:/msys64/ucrt64/bin/gcc`）

```sh
gcc -O2 -Isrc/services -o m17/out/m17_demod_test.exe m17/c/m17_demod_test.c src/services/m17_demod.c src/services/m17_core.c
gcc -O2 -Isrc/services -o m17/out/m17_core_test.exe m17/c/m17_core_test.c src/services/m17_core.c
gcc -O2 -Isrc/services -o m17/out/c2dec_run.exe m17/c/c2dec_run.c src/services/c2dec.c
gcc -O2 -Isrc/services -o m17/out/c2enc_run.exe m17/c/c2enc_run.c src/services/c2enc.c src/services/c2dec.c
gcc -O2 -Isrc/services -o m17/out/m17_tx_test.exe m17/c/m17_tx_test.c src/services/m17_core.c
```

- `m17_demod_test in.raw [out.c2]`：8 kHz 16 位录音 → 解调 → 帧。`data/real_stream.raw`（收音机录的流，中途开始）、
  `data/call_start.raw`（一次发射的开头，前后共 1.5 s）。
- `py/m17_sim.py frames.txt out.raw [--snr --dc --ppm]`：按帧文件仿真 8 kHz 鉴频器录音（40 ms 标准前导码、RRC α=0.5、
  噪声、直流、时钟偏差）。
- `m17_core_test in.dibits [out.c2] [--preamble] [--gap FROM N] [--flip K]`：直接喂 dibit，可模拟擦除和误码。
- `c2dec_run in.c2 out.raw`；`py/c2_3200_model.py`：Codec 2 3200 的浮点模型（与参考解码器核对过）；
  `py/c2_check.py ref.raw ours.raw`：比较两份解码输出。
- `c2enc_run in.raw out.c2`：定点编码器；`py/c2_3200_enc_model.py in.raw out.c2 [ref.c2]`：编码器的浮点模型，
  `--cmp ours.c2 ref.c2` 逐字段比较两份位流（参考位流由热点上的 libcodec2 编出，`out/speech/*.c2`）。
- `m17_tx_test in.c2 out.txt [SRC]`：发射方向组帧，输出与 `py/m17_frames.py` 同格式（逐字节相同），并送回接收器
  检查全部帧、LSF、EoT。

## 测试信号（热点）

- `data/m17_tone1k.txt`：纯单音（约 1 kHz，33 dB），由 `py/c2_tone.py` 搜索出的 Codec 2 帧 `data/tone1k.c2`
  经 `py/m17_frames.py` 打包；`data/m17_zh.txt`：thchs30 中文语音 10 段（96.6 s），参考库编码后打包。
- 热点上用 MMDVM 的 M17 模式逐帧发送（pistar-signal-gen 的发射器加了 M17 帧类型），见 ai repo 的
  `OpenDM32/notes/dm32-m17-2026-11-19.md`。热点开始发射时会丢掉文件开头的 LSF，所以测试里呼号来自 LICH。
- `py/m17_trials.py FILE N`：连发 N 次，每次后读收音机的计数；`py/m17_monitor.py --port COM22 [--tap] [--raw]`：
  读计数、最后的 dibit、通话开头前后的录音（调试版固件）；`py/af_fit.py`：录音与已知符号对齐，求增益、频偏。
- 收音机上的编解码耗时：接收时 `m17_radio_stats.decode_us_max`，发射时 `m17_tx_stats`；调试版另有分步计时
  `c2dec_prof`、`c2enc_prof`（`py/m17_tx_check.py` 打印后者）。

## 发射测试（两台收音机，调试版）

`py/m17_tx_check.py --tx COM22 --rx COM23 [--seconds 3] [--power low|high] [--dev N]`：两台都在同一单工频率的
M17 模式。发射机用测试信号（`m17_tx_source` = 1：300 Hz → 3 kHz 扫频，2 s 一轮，1/4 满幅，按麦克风的节拍），
按 PTT 若干秒；发射期间不读发射机。之后读接收机的计数和 dibit 抓取（最后 4096 个），在 PC 上解出 Codec 2 帧，
与 PC 编码器对同一扫频的输出逐帧比较；也打印发射机的编码耗时（分步）和接收机解调器拟合的增益（偏移量）。

2026-11-20：COM22 → COM23、COM23 → COM22 都是 LSF、75 个流帧、EoT 全收到，Viterbi 误码 0，抓取里的 26–40 个
Codec 2 帧与 PC 编码器逐位相同；编码每 20 ms 帧 8.2 ms（最长 8.9 ms）；COM22 收 COM23 的增益 1154/符号电平，
热点 M17 在 COM22 上为约 1100（偏移量差约 5%）。发射机的 2 个欠载是 EoT 之后 60 ms 的尾巴（与 NXDN 相同）。

## 弱信号与软判决（2026-11-20）

约 −115 dBm（两台拔天线、发射机最低功率），发射机发测试扫频，`py/m17_rx_check.py --tx COM23 --rx COM22` 实时读接收机
最近 64 个解出的流帧（`m17_stream_log`），每个 Codec 2 帧在 PC 编码器的同一扫频里查对：

| | 收到的流帧 | 静音 | 正确的 Codec 2 帧 |
|---|---|---|---|
| 硬判决（以前） | 96.6% | 13 帧 | 85.3% |
| 软判决 | 96.8% | 0 | 94.8% |

- 软判决：解调器给每个符号的软值（归一化电平 ×16）；编码比特的可信度：高位看正负号离 0 多远，低位（内点 / 外点）看
  |z| 离 2 多远；Viterbi 按可信度计代价。`metric` 仍是硬判决下的错比特数（解出的数据重新编码后比较）。
- 静音门限从 24 提到 64：软判决后 25–48 个错比特的帧 71–75% 是对的；语音流帧没有 CRC，错帧挑不出来，只静音像噪声的帧
  （约 130）。离线对比（同一录音，`m17_demod_test` 加 `-DM17_HARD_VITERBI` 是硬判决版）：86.8% → 97.8%。
- 同步不是瓶颈：实时连续接收时锁定很稳。8 kHz 录音的速率按手册不能提高（FM 模块的固定输出速率）。
