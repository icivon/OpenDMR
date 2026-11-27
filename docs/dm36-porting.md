<!-- Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM -->
<!-- DM36 adaptation by BG2LNY -->

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

DM36 主菜单末尾新增“蓝牙互联”，DM32 不出现该菜单。菜单项：

1. 蓝牙开关：软件开关，关闭时停止音频/连接与 CPS 透传。
2. 蓝牙模式：主动 / 被动；选中该项按 `*` / `#` 切换并立即保存。
3. 蓝牙名称：被动模式显示，主动模式隐藏；默认 `BFDM36`，只允许字母和数字，数字键循环输入，`#` 删除，绿键保存。
4. 蓝牙地址：显示模块 `HOSTDEVICE` 上报的地址；未上报前显示 `None`。
5. 设备列表：主动模式显示，进入后发 `AT+SCAN=1`，选择设备发 `AT+CONN=<addr>`。
6. 蓝牙信息：开发调试页，显示版本、连接状态、开关、模式、名称、地址、最近事件。

设置保存在 DM36 专属 `custom_data` 块 `"BTLT"`，不进 `settings_t`，不影响 DM32 的 108 字节布局。连接建立后发送 `AT+AGHF/SPKEN/MICEN/AUDIO=1`，断开或关闭时发送 `=0`。

#### 被动模式蓝牙 CPS 串口

被动模式下打开蓝牙开关，模块以 `BFDM36`（或已保存名称）等待电脑/手机连接。连接建立后，蓝牙串口的二进制载荷直接进入 OpenDMR 的 CPS 通道：

```text
电脑/手机 --串口原始字节--> LytLink 模块 --+DATA:00,<n> 或 +ADATA:00/01,<n>--> 固件
固件 --AT+DATA=00,<n> 或 AT+ADATA=01,<n>--> LytLink 模块 --串口原始字节--> 电脑/手机
```

固件按请求所在通道原路回复。帧头之后是 `n` 字节二进制载荷，载荷中的 `\r` / `\n` 不参与帧解析；单帧上限 `1536` 字节（`CABLE_LINK_BUFFER_SIZE`）。网页端需要一次发送完整 CPS 请求，等待同一通道的回复后再发下一条。

CPS 命令沿用有线编程线协议，无需新增业务命令：

| 命令 | 用途 |
|---|---|
| `R` | 读取 Flash / EEPROM / 机型信息 / 语言区，频率、信道、设置项都通过这些地址读取 |
| `X` | 4 KB 扇区写入，Prepared → Data → Sector；写频、改设置走这条链 |
| `C` | 写完成后保存设置、设置日期时间、关闭 CPS 界面 |
| `D` | CRC、调试、固件升级等扩展命令；普通 CPS 读写不使用 |

HTML/JS 端只需发送与接收原始字节；帧封装由 LytLink 模块和固件完成，不要把 AT 文本帧暴露给 CPS 业务层。

已分析实际 CPS 源码 `C:\Users\Administrator\Desktop\opendmr-cps-develop`（v0.13.0，入口 `js/radio/link.js`）：

- 当前只有 `navigator.serial`（Web Serial），没有任何 `navigator.bluetooth` / Web Bluetooth SPP 实现；蓝牙入口需要另行适配传输层，但 CPS 的请求/响应语义可直接复用。
- `send()` 先清空接收缓冲，再写完整请求；`receive(n)` 按命令规定的长度取字节，超时 2000 ms，读/写按 1024 字节分块，错误重试 3 次。
- `D S <divisor>` 提速协商会要求电台改 UART0 波特率并 `reopen()` 串口。LytLink 的 UART2 固定 115200，因此固件在蓝牙通道上把 `D S` 当作“老固件”回 `D S 0`，客户端会退回 115200，不会切换速率。
- `C` 命令只等 1 字节且忽略超时，`R` / `X` / `D K` 都有明确响应头，现有 `cable_link` 引擎已能提供。

原厂文档 `dm36-stock-firmware-analysis.md` 的 AT 表确认了 `AT+DATA=00,<n>` / `AT+ADATA=01,<n>` 发送命令；响应侧只列出 `+DATA:00` / `+ADATA:00` / `ADATA` / `DATA`，**没有直接给出 `,<n>` 长度字段**。当前解析中的 `,<n>` 来自本移植的实现约定，仍需 UART2 抓包证实。文档也没有固定芯片型号或版本串，`AT+VER?` 的 `VER:` 只能在实机上读取。

健壮性处理（全部在 DM36 独立文件内，DM32 不受影响）：

- UART2 发送由静态互斥保护；1536 字节 CPS 回复不再在临界区内阻塞系统 tick，音频命令组合同一把锁，避免与 CPS 帧交错。
- 超长帧、蓝牙关闭状态下的帧整帧丢弃，不再截断成“看起来合法”的 CPS 请求。
- `SCAN:` / `HOSTDEVICE` 地址解析限定在字符串长度内；扫描多个地址时正确推进指针，避免重复解析同一地址。
- 回复通道变量改为 `volatile`，避免蓝牙任务和 CPS 任务之间读到陈旧值。
- 蓝牙请求会被打上 2 秒有效标记，`BOARD_CPS_REPLY_OUT` 只把对应的回复送回 LytLink；有线 CPS 的回复不再串到蓝牙。
- 模块的 `READY` 上报会重新下发版本/增益/名称/自动连接命令，避免模块比电台晚启动时配置丢失。
- 关闭蓝牙时用 `CONN` 中解析出的远端地址发送 `AT+DISC=<addr>`；PTT 仍由机器自身控制，未接入耳机 `PTTKEY`。

待确认：

- 串口抓 UART2（115200）中 `AT+VER?` 的 `VER:` 响应，记录模块版本。
- 被动模式默认名 `BFDM36` 是否能被电脑/手机搜索到；名称修改是否即时生效。
- 蓝牙耳机话筒/喇叭只做音频；模块模拟音频到音频开关/功放的硬件路由仍需实机确认。
- 网页端实测 `+DATA` / `+ADATA` 两条通道的收发与长包边界。
- 抓 UART2 确认 `+DATA:00,<n>` / `+ADATA:00/01,<n>` 的长度字段格式；当前按该格式解析，尚未由实机响应证实。
- 有线和蓝牙 CPS 同时使用时的请求来源已隔离，但仍建议实机确认两边不会互相干扰。

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

## 3. DM36 独有文件标注清单

以下文件顶部保留原版权行：

```text
Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
```

下一行增加英文适配说明：

```text
DM36 adaptation by BG2LNY
```

Python 脚本保留 shebang 为第一行，版权/适配说明从第二行开始；Markdown 文件使用 HTML 注释。共享源码文件不在本清单内，也未因本次标注修改。

### 源码 / 头文件

- `src/board/board_dm36.h`
- `src/board/gps_stub.c`
- `src/board/dm36/trx_dm36.c`
- `src/board/dm36/uart2_dm36.c`
- `src/board/dm36/uart2_dm36.h`
- `src/board/dm36/bluetooth.c`
- `src/board/dm36/bluetooth.h`
- `src/board/dm36/bluetooth_menu.h`

### 文档

- `docs/dm36-porting.md`
- `docs/dm36-stock-firmware-analysis.md`
- `docs/dm36-stock-fd6818-writes.md`

### 调试工具

- `tools/debug/dm36_keyscan.py`
- `tools/debug/dm36_keywatch.py`
- `tools/debug/dm36_rfwatch.py`
