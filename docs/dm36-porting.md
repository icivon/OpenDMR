# DM36 剩余工作清单

> 本文只保留未确认、未解决的问题，以及实机验证步骤和复核命令。已确定、已修复的历史过程不再记录。
>
> 目标设备：DM36（HR_C7000 / CK803S，FD6818B，240×320 ST7789，无 GPS，无独立 P1/P2）。原厂寄存器、校准数据与蓝牙协议分析仍在 `docs/dm36-stock-firmware-analysis.md`、`docs/dm36-stock-fd6818-writes.md`。

## 1. 待实机验证

### 1.1 合成器与射频

当前 DM36 产物：`src/build/opendmr_dm36_BFUV32.bin`。刷入后按以下顺序确认：

1. 机型页看合成器状态；不应显示 `no lock`。
2. 读芯片 ID：`fdr 0x00` 期望 `0x4829`。若异常，先用 `fda` 导出寄存器，对照原厂表。
3. FM 12.5 kHz 收听：确认啸叫/杂音是否消失。
4. FM 发射：另一台电台确认能否听到话音；声音过小再调 `src/board/dm36/trx_dm36.c` 的 `set_mic_gain()` 台阶。
5. 数字模式（DMR / P25 / NXDN / M17）接收：有信号时确认是否显示 ID、是否有声音。
6. 数字模式发射：确认对端是否能解出话音。

回退/调参点：

| 项 | 位置 | 调整方向 |
|---|---|---|
| DMR DAC 增益 | `trx_dm36.c` 的 `0x48 = 0xB2FD` | 若数字接收失真，先回退 `0xB2F5` |
| DMR `0x2B` | `trx_dm36.c` | 原厂 `0x00E0`，上游 FD6818B `0x0700`；数字接收异常时二分 |
| 带宽字 | `trx_dm36.c` 的 `set_bandwidth()` | FM 25k `0x3028`、12.5k `0x3048`、6.25k `0x205C`、DMR `0x0048` |
| AM 带宽 | `trx_dm36.c` | 当前走 12.5k；原厂另有 `0x2028` / `0x205C`，待实听后决定 |
| 发射校准字节 | `trx_dm36.c` 的 `0x40` | 当前沿用上游 `0x34EA`；需读 DM36 校准扇区对应字节后再改 |
| 数字发射话音 | 编解码器增益 | 若 P25/NXDN/M17 发射过弱，测 `GCR_MIC1` / `GCR_ADCL` |

### 1.2 按键

待实机确认 `*`（减小/上一个）和 `#`（增大/下一个）在以下位置是否生效且方向正确：

- 通用设置列表
- 联系人详情：呼叫类型、时隙覆盖
- 信道 / VFO 详情
- 快捷菜单
- 电台信息：电池图 bars / dots

若 `*`/`#` 无效，先用 `fkey.py star` / `fkey.py hash` 注入 UI 事件：

- 注入有效、物理键无效：查矩阵位置和 `src/board/board_dm36.h` 的 `dm36_key_index()`。
- 注入也无效：查对应页面的改值事件分支。

数字键仍需在实机上逐个确认；当前 `#` 与数字键可能都从矩阵位置 8 上报。

有意不把 `*`/`#` 加成改值键的位置：卫星 PASS 页（`*` 切换 PASS / PASSES）、FM 收音机（`*` / `#` 搜台）、文本/号码输入、扫描、键位设置、主菜单 `*`/`#` 锁定。

### 1.3 FM 收音机无声

待确认：

- DM36 是否确有 RDA5807。
- I2C 引脚是否仍是 `0x19` / `0x1A`。
- RDA5807 音频到功放的使能/切换是否由 OpenDMR 正确控制。

### 1.4 音量旋钮无效

待确认：

- DM36 旋钮 A/B 脚（当前沿用 DM32 的 `0x27` / `0x26`）。
- 需求边界：OpenDMR 当前没有“旋钮调音量”功能，首页旋钮用于调信道；若 DM36 原厂是音量旋钮，需要单独定义功能。

### 1.5 蓝牙 / LytLink

待确认：

- 串口抓 UART2（115200）中 `AT+VER?` 的 `VER:` 响应，记录模块版本。
- 蓝牙音频如何经音频开关/功放接到模块模拟脚。
- `+DATA:` / `+ADATA:` 与 CPS 请求/回复的完整桥接。

### 1.6 其他未测项

- CPS 机型码：当前沿用 11，需观察 CPS 行为后决定。
- 电池 ADC 系数：当前沿用 DM32 值，需实测校准。
- 键盘背光、旋钮、个别矩阵数字键的最终引脚/行为，以实机为准。

## 2. 复核命令

### 2.1 构建

Git Bash 下：

```bash
cd /e/peng/OpenDMR/src
export PATH="/e/peng/OpenDMR/toolchain/MinGW/msys/1.0/bin:/e/peng/OpenDMR/toolchain/MinGW/csky-elf-noneabiv2-tools/bin:$PATH"
make clean
make CROSS=csky-abiv2-elf- BOARD=dm36
```

DM32：

```bash
make clean
make CROSS=csky-abiv2-elf-
```

切换 `dm32` / `dm36` 前必须 `make clean`：`BOARD` 宏不在 `.d` 依赖里，不清理会链接到旧目标的对象文件。

### 2.2 调试

```bash
py tools/debug/dm32dbg.py --port COM6 info
py tools/debug/dm32dbg.py --port COM6 fdr 0x00
py tools/debug/dm32dbg.py --port COM6 fda --save dm36.txt
py tools/debug/dm32dbg.py --port COM6 fdcdiff dm32cache.txt dm36.txt

py tools/debug/fkey.py star
py tools/debug/fkey.py hash
py tools/debug/fkey.py sk1
py tools/debug/fkey.py sk2
py tools/debug/fkey.py ptt

py tools/debug/fshot.py shot.png --scale 2
```

读 GPIO_EXT：

```bash
py tools/debug/dm32dbg.py --port COM6 mr 0x14020050 4 --width 4
py tools/debug/dm32dbg.py --port COM6 mr 0x14100050 4 --width 4
py tools/debug/dm32dbg.py --port COM6 mr 0x14110050 4 --width 4
```

换算：`pin = bank 起始 pin + bit 序号`；GPIO A/B/C 起始 pin 分别为 `0` / `32` / `64`。
