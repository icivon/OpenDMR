# DM36 原厂固件分析结果（供后续人工修复 Bug 参考）

> 来源：`C:\Users\Administrator\Desktop\36\dm36-program-20261125-1933.bin`（1 MB 程序 Flash）
> 与 `dm36-data-20261125-1940.bin`（16 MB 数据 Flash），版本串 `DM36.NRFB.01.026`（偏移 `0xB0FBC`）。
>
> 本文只保留**从原厂固件直接提取的事实**和**可复现的分析方法**。移植代码里的应用结果见 `dm36-porting.md`
> 第 10.3 / 10.6 / 10.7 / 10.8 节；本文是它们的原始证据与地址索引，供后续人工核对、继续排查用。
>
> 分析日期：2026-11-26。

---

## 1. 分析方法（可复现）

### 1.1 反汇编

```powershell
cd e:\peng\OpenDMR
& toolchain\MinGW\csky-elf-noneabiv2-tools\bin\csky-elf-noneabiv2-objdump.exe `
  -D -b binary -m csky --adjust-vma=0x03000000 `
  'C:\Users\Administrator\Desktop\36\dm36-program-20261125-1933.bin' > stock.dis
```

- `--adjust-vma=0x03000000`：程序 Flash 的 XIP 基址；**文件偏移 = VMA - 0x03000000**。
- 例：VMA `0x0300ed96` 的字节在文件 `0xed96`。
- objdump 输出的 4 字节指令字段是**16 位半字交换**的（如显示 `51ffffe3`，实际字 `0xe3ffff51`），阅读操作数以反汇编文本为准。
- 分支目标显示偶尔丢高位（如 `bsr 0x2c3d8` 实为 `0x302c3d8`），必要时按字节自行核算。
- PowerShell 重定向生成 UTF-16 文本；Python 读取时用 `encoding='utf-16'`。

### 1.2 提取 FD6818 寄存器写入

原厂所有 FD6818 写寄存器都经过**同一个函数**（objdump 标号 `0x2c3d8`，VMA `0x302c3d8`），调用约定是
`r0 = 寄存器号, r1 = 16 位值`。扫描模式：

```
movi r0, <reg>
movi r1, <value>        # 有些后面紧跟 lsli r1, r1, N，实际值 = value << N
bsr  <write_fn>
```

找到 231 处直接调用，寄存器范围 `0x00..0x7E`。完整列表（231 行，含 VMA）在
`docs/dm36-stock-fd6818-writes.md`。**注意带 `lsli` 的值**：`0x10<<2`、`0x14<<2`、`0x28<<4`、
`0x29<<8`、`0x2A<<7`，表里已标注。

### 1.3 定位字符串引用

字符串在固件里以 ASCII 存放（文件偏移 = VMA - `0x03000000`）。代码通过 `lrw rX, <literal>` 从字面量池
加载字符串地址；因此先 `struct.pack('<I', VMA_of_string)` 在 bin 里找到池项，再在反汇编里找引用该池
地址的 `lrw`。AT 命令字符串即这样定位（见第 4 节）。

---

## 2. FD6818B（BK4829, ID 0x4829）初始化表

代码位置：VMA `0x0300ed96`（`push r15` 后是一串 `movi r0/r1; bsr 0x2c3d8`）。复位序列与其他
BK4819/BK4829 一致：`0x00=0x8000` 再 `0x00=0x0000`。**原厂从不读 `0x00` 判断型号**——复位后直接写表。

按写入顺序还原的实际值（`lsli` 已算入）：

| reg | 值 | reg | 值 | reg | 值 |
|---|---|---|---|---|---|
| 0x00 | 0x8000 → 0x0000 | 0x19 | 0x1041 | 0x48 | 0xB386 |
| 0x37 | 0x9D1F | 0x1C | 0x0422 | 0x49 | 0x2A32 |
| 0x13 | 0x03DF | 0x1D | 0x2AAB | 0x4A | 0x5430 |
| 0x12 | 0x03DB | 0x1E | 0x4C58 | 0x4D | 0xA015 |
| 0x11 | 0x033A | 0x1F | 0x165A | 0x4F | 0x2A28 |
| 0x10 | 0x0318 (0xC6<<2) | 0x25 | 0x6DBA | 0x53 | 0x2028 |
| 0x14 | 0x0210 (0x84<<2) | 0x28 | 0x0A40 (0xA4<<4) | 0x73 | 0x6681 |
| 0x2C | 0x0022 | 0x29 | 0xAA00 (0xAA<<8) | 0x77 | 0x88EF |
| 0x2F | 0x9890 | 0x2A | 0x6600 (0xCC<<7) | 0x7B | 0x73DC |
| 0x3A | 0x9A7C | 0x3E | 0x94C6 | 0x7E | 0x303E |
| 0x3F | 0x07FE | 0x40 | 0x34F0 | 0x46 | 0x6050 |
| 0x4C | 0xE520 | 0x4E | 0x1F10 | 0x36 | 0x003F |

之后是 16 个 `0x09` 校准寄存器（`0x006F / 0x106B / 0x2067 / 0x3062 / 0x4050 / 0x5047 / 0x603A /
0x702C / 0x8041 / 0x9037 / 0xA025 / 0xB017 / 0xC0E4 / 0xD0CB / 0xE0B5 / 0xF09F`），与 OpenDMR 现有表一致。

### 2.1 与 OpenDMR `init_settings_b` 的重大差异（收发失败根因）

OpenDMR 先前用的是 UV-K1/K5v3 的 BK4829 表，PLL/VCO 与原机不同：

| reg | 原厂 DM36 | OpenDMR（旧） | 备注 |
|---|---|---|---|
| 0x1C | 0x0422 | 0x07C0 | PLL |
| 0x1D | 0x2AAB | 0xE555 | PLL/VCO |
| 0x1F | 0x165A | 0xC65A | VCO 偏置，bit15 不同 |
| 0x25 | 0x6DBA | 0xC1BA | 电荷泵 |
| 0x28 | 0x0A40 | 0x0B40 | — |
| 0x2C | 0x0022 | 0x1822 | — |
| 0x2F | 0x9890 | 未写 | 音频路由 |
| 0x46 | 0x6050 | 0x600A | 音频 |
| 0x48 | 0xB386 | 0x33EF / 0xBBBF | AGC/音量 |
| 0x49 | 0x2A32 | 0x2AB2 | AGC 阈值 |
| 0x4E | 0x1F10 | 0x4708 | — |
| 0x4F | 0x2A28 | 0x302F | — |
| 0x73 | 0x6681 | 未写 | AFC |
| 0x7E | 0x303E | 0x302E | AGC 模式 |

结论：PLL/VCO 写错⇒频率合成器不锁定⇒完全无法收发。**已经在 `fd6818.c` 用 `init_settings_dm36[]`
修正并强制选用（不依赖读 ID）**；若实测仍有明显频偏/不锁定，首先核对本表。

### 2.2 模式切换表（仍待实机核对）

| 用途 | VMA | 关键写入 |
|---|---|---|
| DMR 接收 | `0x0300f26e` | `0x43=0x0048`, `0x47=0x6740→0x6943`, `0x4A=0x5450→0x5430`, `0x13=0x03ff/0x03df`, `0x48=0xB2F5`, `0x7E=0xB03F`, `0x2B=0x00e0/0x00c0`, `0x28=0x00a4` |
| 发射 | `0x0306a0c6` | `0x50=0x3B20`（FM）/ `0xBB20`（DMR）, `0x30=0xC3FA`→`0xC1FE`, `0x47=0x6040`, `0x2B=0x0005`, `0x58=0x3C03`, `0x53=0x2028`, `0x70=0xE0E0` |
| 带宽 | `0x0300e87a` | `0x43=0x3048 / 0x3028 / 0x205C / 0x2028` |
| 尾音 | `0x0300e988` | `0x07=0x046F`；`0x52=0x828F / 0xA28F / 0xC28F / 0xE28F`，结束 `0x028F` |
| 音频模式切换 | `0x0300e282` | `0x2E=0x8608 ↔ 0x9608`, `0x2F=0x1890 ↔ 0x9890`（配合 `bsr 0x300e120` 的模式号 1/2/3/0） |
| 静音/AF 输出 | `0x0300e93a` | `0x47=0x6943` 配 `0x7E=0x3006`；`0x47=0x6042` 配 `0x7E=0x302E` |

注意：OpenDMR 的 `dmr_settings` 用 `0x43=0x4048 / 0x47=0x0140 / 0x7E=0x303F`，与原厂 DMR 表不同；
DM36 分支已按原厂值建 `dmr_settings_dm36[]`，但**这些值是否最佳仍需实机听/解码验证**。

### 2.3 已应用的修复（避免重复排查）

- `src/board/dm36/fd6818_dm36.c`：DM36 专属 FD6818 驱动（独立文件），含原厂 init/post-init/FM/DMR 表；
  `fd6818_init()` 在 DM36 下不读 ID、`BANDWIDTH_DMR=0x0048`；DM36 构建时由 Makefile 替换共享 `fd6818.c`。
- `docs/dm36-porting.md` 第 10.7 节记录了差异表与编译结果。

---

## 3. 按键音“破音”相关（第 10.6 节证据）

- 原厂固件同样使用 HR_C7000 提示音 FIFO `0x16000980`（字符串/代码在 `0x0300d47c`、`0x03021184`、
  `0x03021378`、`0x03021f06`），说明提示音通道选择本身没问题。
- 原厂提示音启动时：清 FM 音频位（`bsr 0x300c2dc`, r0=0）、置提示音位（`bsr 0x301d520`, r0=1），
  与 OpenDMR `modem_beep()` 一致。
- 原厂音频模式切换会成对写 FD6818 `0x2E/0x2F`（见 2.2），OpenDMR 原先未覆盖；DM36 的 FD6818B AF 输出
  未被静音，与基带提示音叠加⇒破音。
- 已修：通过 `board.h` / `board_dm36.h` 的 `BOARD_BEEP_PATH_AF_OUT` 钩子，`radio_beep_path()` 在 DM36 下静音
  FD6818 AF；`BOARD_AMP_START_MS` 把 `AMP_START_MS` 从 75 ms 提到 120 ms（DM36 功放启动更慢）。
  DM32 构建中两个钩子为默认空/75 ms，共享源文件只有一行钩子调用。

---

## 4. 蓝牙模块（LytLink）—— 外接 AT 命令模块

### 4.1 重要结论

- 固件里**没有独立蓝牙芯片型号串**；模块品牌/协议名为 **LytLink**：
  - 菜单项字符串：`0x0BFC50 "LytLink"`、`0x0C11B4 "LytLink Ver:"`、`0x0C1518 "LytLink Not Open!"`
  - 确切芯片型号需看模块丝印或 `AT+VER?` 的 `VER:` 响应。
- 这是**外接蓝牙 PTT/音频模块**，通过 UART AT 命令控制；不是 HR_C7000 内置。

### 4.2 硬件接口（已从代码确认）

- **UART2 @ `0x14090000`，115200 8N1**。
  - 字节发送函数 VMA `0x03061a2c`：`movih r2, 5129`（=`0x1409`），轮询 `USR(r2+0x7c) & 2`，写 `THR(r3+0)`。
  - UART2 初始化 VMA `0x301c990`：分频 `(clock_apb>>4)/115200`（立即数 `225<<9`）。
  - 原厂没有注册 UART2 中断（同 OpenDMR 现实现：轮询接收）。
- 模块电源/复位相关：初始化 VMA `0x030619ec` 附近按状态控制 GPIO `350`(`175<<1`) 与 `48`；
  具体脚位未实测确认，暂未改 `board_dm36.h`。

### 4.3 AT 命令集（字符串 `0x0BA948`–`0x0BAB90`）

| 类别 | 命令（发→模块） | 响应/主动上报（模块→机） |
|---|---|---|
| 版本/就绪 | `AT+VER?`, `AT+AUTOCONN?` | `VER:`, `READY` |
| 配对/连接 | `AT+LENAME=`/`?`, `AT+PIN=`/`?`, `AT+DNAME?`, `AT+CONN=<addr>`, `AT+DISC=<addr>`, `AT+SCAN=1/2`, `AT+LESCAN=1/2`, `AT+LEDISC=<addr>` | `LENAME:`, `PIN:`, `SCAN:001122334455`, `CONN`, `DISC`, `HOSTDEVICE` |
| 音频 | `AT+AGHF=0/1`（HFP 开关）, `AT+MICEN=0/1`, `AT+SPKEN=0/1`, `AT+AUDIO=0/1`, `AT+MICGAIN=15`, `AT+SPKGAIN=25` | `HFREQ` |
| PTT | — | `PTTKEY`, `PTTBATT`, `3900mV`/`3800mV`/`3700mV`/`3500mV`/`3300mV` |
| 写频/透传 | `AT+DATA=00,<n>`, `AT+ADATA=01,<n>` | `+DATA:00`, `+ADATA:00`, `ADATA`, `DATA` |

其他相关代码位置：AT 命令组装 `0x03062764`（如 `AT+CONN=`/`AT+LENAME=`/`AT+PIN=`），
命令解析/状态 `0x03061a44`，任务入口附近 `0x03061b50`，字符串表 `0x030ba948` 起。

### 4.4 已实现与待办

已实现（全部集中在 `src/board/dm36/`，DM36 构建才编译，DM32 不收集）：
- `src/board/dm36/uart2_dm36.c/.h`：`uart2_init/uart2_write/uart2_read`
- `src/board/dm36/bluetooth.c/.h`：启动发 `AT+VER?`/`AT+AUTOCONN?`，解析 `CONN/DISC`，
  `bluetooth_audio()` 与 `bluetooth_write_data()` 接口
- `bluetooth_start()` 在 DM36 独立的 `fd6818_dm36.c` 中调用，共享 `main.c` 与上游完全一致

**待人工/实机**：
1. 音频：模块模拟音频脚与音频开关/功放的 GPIO 路由尚未接入；现只完成模块的 HFP/SPK/MIC 控制命令。
2. 写频：`+DATA:` 帧尚未接到 `cable_link` 的 CPS 通道；现只有 UART2 透传接口。
3. 抓 UART2（115200）看 `AT+VER?`→`VER:` 响应，获取模块固件版本/型号。

---

## 5. 校准数据（数据 Flash，第 10.3 节证据）

- 原厂校准扇区 `0xA2000`（末字节 `0x02`，`0x300`/`0x328` BCD 频点有效）。
- OpenDMR 校准副本 `0x710000` 在该备份中全 `0xFF`，首次启动复制；`factory_cal.c` 搜索规则直接命中。
- 关键字段原厂值：`TX adjust=113`, `UHF bias=80 78 71 69 66`, `RX adjust=497`,
  `rx_levels=6/36`, UHF 频点 `400/417.5/435.5/452.5/470`, VHF `136/145.5/155.5/164.5/174`。
- `0x604b` 处 OpenDMR settings 区全 `0xFF`，无历史设置；`beep_volume_divider` 走默认 4。

---

## 6. 仍未解决 / 建议的下一步

| 项 | 状态 | 下一步 |
|---|---|---|
| FD6818 软 SPI dump | 未采集 | `fdr 0x00` 应读到 `0x4829`；否则查 `SCL=0x07/SDA=0x08/SCN=0x18` 与 mux |
| FM/DMR 收发 | 待定性 | 刷入含原厂表的固件，另一台电台定性收发；失败先对 `docs/dm36-stock-fd6818-writes.md` 核寄存器 |
| PA/LNA/音频开关脚 | 未最终确认 | `mr` 读 GPIO 变化；当前沿用 DM32 宏 |
| SK1/SK2 短/长按无反应 | 未解决 | 查 `keys.c`/`input.c`/`keymap.c`，用 `fkey.py sk1` 分层定位 |
| PTT 有界面无 RF | 未解决 | 先确认 FD6818B 表已生效，再查 `radio_tx.c` PA 脚与 `fd6818_transmit()` |
| 蓝牙音频/写频端到端 | 基础链路已建 | 见 4.4 三项；抓 UART2 与确认音频开关 GPIO |
| 按键音破音 | 已修，待实机 | 刷固件确认；仍破音则查 `0x2E/0x2F` 与功放启动时间 |

---

## 7. 文件索引

| 文件 | 内容 |
|---|---|
| `docs/dm36-porting.md` | 移植流程与实测结果（10.3 校准、10.6 按键音、10.7 FD6818B、10.8 蓝牙） |
| `docs/dm36-stock-fd6818-writes.md` | 原厂 231 条 FD6818 寄存器写入完整列表（VMA/reg/value） |
| `docs/fd6818-registers.md` | BK4819/BK4829 寄存器含义参考 |
| `src/board/dm36/fd6818_dm36.c` | 已应用的 DM36 专属寄存器表（独立驱动） |
| `src/board/dm36/bluetooth.c` | 已实现的 LytLink AT 命令框架 |
| `src/board/dm36/uart2_dm36.c/.h` | DM36 专属 UART2 蓝牙链路驱动 |
