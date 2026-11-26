# DM36 移植可行性与执行流程

> 目标设备：DM36（屏幕同 DM32；侧键 PTT / SK1 / SK2 数量相同；无 GPS；无左右 P1/P2 键；RF 芯片 FD6818B；DMR/AMBE 芯片 HR_C7000；外部存储同 DM32）
>
> 本文基于 `docs/` 现有文档与 `src/` 源码静态分析编排，不含任何原厂资料推断。
>
> 当前可用资源（2026-11-25 更新）：已有 DM36 Flash 备份文件并可提供；已有授权许可；射频验证可用另一台电台完成定性收发/互通。
>
> **原厂固件提取的原始数据、地址索引与分析方法见 `docs/dm36-stock-firmware-analysis.md`**（含 FD6818B 完整寄存器表
> `docs/dm36-stock-fd6818-writes.md`、LytLink 蓝牙 AT 协议、校准数据），供后续人工修复 Bug 时核对。

---

## 1. 结论

**可行，但不是零成本移植。**

| 判断维度 | 结论 |
|---|---|
| SoC / CPU | HR_C7000（CK803S）相同，内核与时钟链已跑通 |
| 屏幕 | 240×320 ST7789、I8080 8 位并口相同，`panel.c/.h` 可复用 |
| 程序 Flash / XIP | 已正常启动，地址与固件头无需变动 |
| 数据 Flash | 型号/容量/布局相同，`spi0.c`/`flash.c` 可复用 |
| DMR 引擎 / 声码器 | HR_C7000 相同，DMR/P25/NXDN 核心可复用 |
| **按键** | **引脚号与 pin mux 写死为 DM32，当前完全失效——需适配** |
| **收发** | **FD6818 软 SPI / PA-LNA 引脚、出厂校准表需适配** |
| GPS | 默认关闭，删除不影响收发 |
| P1/P2 | 本固件**无独立 P1/P2 按键**，SK1/SK2 即左右侧键；DM36 缺少 P1/P2 不产生移植工作 |

**现象反推**：开机屏幕正常 ⇒ CPU、时钟、程序 Flash XIP、屏驱动、framebuffer 全部正常。故障被收敛到两个纯板级层：**GPIO 引脚/mux（按键）** 与 **射频前端（收发）**。

**射频芯片区分**：DM36 使用 **FD6818B / BK4829（ID `0x4829`）**，DM32 使用 **FD6818 / BK4819（ID `0x4819`）**。驱动已包含两套初始化表并在 `fd6818_init()` 中按寄存器 `0x00` 自动选择（`src/drivers/fd6818.c:156-240, 364-374`），因此芯片型号本身不是阻塞项；DM36 射频采集时应以 `0x4829` 为期望 ID。

---

## 2. 板级抽象（代码层已完成，见第 9 节）

原本全仓没有 DM32/DM36 型号识别、板级 ID 读取或编译期 `BOARD_*` 分支，所有硬件配置以字面值硬编码。现已建立 `src/board/` 独立目录，DM32 为默认、DM36 为薄覆盖层：

```
src/board/board.h          # 按 BUILD 宏选择映射，默认 DM32
src/board/board_dm32.h     # 原有字面值原样集中
src/board/board_dm36.h     # include dm32 + 只覆盖已知差异
src/board/gps_stub.c       # DM36 无 GPS 时的 API 空实现
```

共享源文件只做"字面值 → 板级宏"的一行替换（13 个文件，共 +82/-66 行），上游更新时冲突面很小。仍需注意的非板级项：

- 镜像头 `BFUV32-V2`（`src/Makefile`）：DM36 是否沿用待实机确认
- 初始化流程固定（`src/app/main.c:93-146`）：无运行时型号判断，靠编译期宏切换

---

## 3. 硬件依赖清单（逐项核对）

### 3.1 按键（当前完全失效，最高优先级）

读取方式为 **GPIO 直读 + 5×4 矩阵软件轮询**，无中断、无 ADC 按键。

```c
// src/drivers/keys.c:44-51
#define PIN_KEY_1    49 // columns
#define PIN_KEY_3    51
#define PIN_KEY_4    52
#define PIN_PTT      43
#define ROWS         5
#define COLUMNS      4
#define ROWS_AS_GPIO 0xFE00 // IO_DIPLEX2: LCD_DB0-DB4
```

| 项目 | DM32 硬编码 | 位置 |
|---|---|---|
| SK1 / SK2 | GPIO 51 / 52（复用列 2 / 列 3） | `src/drivers/keys.c:80-85` |
| PTT | GPIO 43，16×100 µs 防抖 | `src/drivers/keys.c:88-102` |
| 矩阵行 | GPIO 71–75 = LCD DB0–DB4，扫描时 `IO_DIPLEX2 |= 0xFE00` 并占 LCD 总线 | `src/drivers/keys.c:113-148` |
| 行定义 | `PIN_KEYPAD_ROW0=71`, `ROW4=75` | `src/hal/hw.h:123-124` |
| mux / DDR / 上下拉初值 | `IO_DIPLEX0=0xFFFDFD80`, `IO_DIPLEX1=0xFFFFFE07`, `IO_DIPLEX2=0x0FC80100` | `src/hal/gpio.c:67-98` |
| 旋钮 A/B | 0x27 / 0x26（TIMER2 的 1 ms ISR 轮询） | `src/drivers/rotary.c:38-39` |

**最可疑点**：DM36 屏幕相同（I8080 总线正常）但侧键 GPIO 编号或 mux 只要变一个，读到的全是高电平，即"按键完全无反应"。若 DM36 行线不再与 LCD 数据线复用，`ROWS_AS_GPIO` 切换反而会破坏显示。

### 3.2 收发（当前失效，按怀疑度排序）

**1) 出厂校准表找不到（首怀疑）**

FD6818 的 4FSK 偏置、RX 调整、PA 功率 DAC 来自数据 Flash 中的原厂校准扇区，按内容搜索：

```c
// src/services/factory_cal.c:39-42
#define SEARCH_START 0x1000
#define SEARCH_END   0xC9000
#define SECTOR_SIZE  4096
#define SECTOR_TAG   0x02
```

字段偏移：`0x016` 晶振、`0x017/0x0A6` 4FSK 偏置、`0x027` RX 调整、`0x040/0x0D0` PA 功率、`0x20A/B` RX 电平、`0x300/0x328` 校准频点（`src/services/factory_cal.h:35-58`）。找不到时用 `DEFAULT_TABLE[]`（`src/services/factory_cal.c:78-90`，从两台 DM32 抄的折中值）。

`docs/factory-calibration.md:22-23` 记录 issue #142：原厂扇区丢失 → 4FSK 偏置退回 `0xFF` → 频偏大三倍多 → FM 正常但 DMR/P25 有载波对方解不出。DM36 校准数据格式/tag/扇区只要不同，就会静默退化，**现象吻合**。

**2) FD6818 软 SPI 引脚 / 时序**

FD6818 走 GPIO 软件 bit-bang（硬件 SPI0 专用于 25Q128）：

```c
// src/drivers/fd6818.c:45-49
#define PIN_SCL 0x07
#define PIN_SDA 0x08
#define PIN_SCN 0x18
#define PIN_GP0 0x3B
#define PIN_GP1 0x3A
```

写 `delay(5)` / 读 `delay(10)`（`fd6818.c:191-229`）。PA/LNA/音频开关：

```c
// src/services/radio_internal.h:49-54
#define PIN_LNA_VHF  0x22 // R5_V_SW
#define PIN_LNA_UHF  0x24 // R5_U_SW
#define PIN_PA_VHF   0x37
#define PIN_PA_UHF   0x23
#define PIN_AUDIO_SW 0x2F
```

**3) FD6818B 寄存器差异**

DM36 是 FD6818B / BK4829（`0x4829`），DM32 是 FD6818 / BK4819（`0x4819`）；驱动已在 `fd6818_init()` 中读取寄存器 `0x00` 并自动选择对应初始化表（`src/drivers/fd6818.c:156-240, 364-374`）。DM36 采集时 `fdr 0x00` 应返回 `0x4829`；如果读到 `0x4819`、全 0 或全 `0xFF`，先查软 SPI 引脚/mux。

`docs/fd6818-registers.md:32-36` 已有跨机型差异证据：另一台机 `0x47` 高位为 `0x7840/0x7040`，本机为 `0x6042/0x6142`。DMR 关键位：`0x50=0xBB20`（bit15 把原始基带送 HR_C7000，否则无 DMR 同步）、`0x43=0x4048`（DMR 带宽），见 `src/drivers/fd6818.c:188-192`。

**4) HR_C7000 侧**

`modem.c` 的 `RF_MODE=0x03269860`、`RF_CONTROL=0x00041F0A`、`SIG_CENTER`、DAC B 偏置来自原厂；`src/dmr/hr_c7000.c:2943-2956` 记录"重复软复位会永久搞死 DMR 接收"。**在 FD6818 通信打通前不要动这里**。

### 3.3 GPS（可安全裁剪）

- 默认 `settings.gps = 0`（`src/services/settings.c:107` → `GPS_MODE_NOT_DETECTED`），GPS 接收机默认不开启
- `src/app/main.c:132` 无条件调用 `gps_start()`，但只注册任务与 UART1，不开接收中断
- 占用资源：电源 GPIO `0x2C`（`src/services/gps.c:59`）、UART1 基址 `0x14040000`、IRQ 10
- 不能只删 `main.c` 那一行：菜单仍会调 `gps_set_enabled()`，会访问不存在的 GPIO/UART。需同时处理 `src/app/gps_screen.c:137-142`、`src/app/general_options.c:258-286`，并给 `gps_read()/gps_has_fix()/gps_position()` 加存根

### 3.4 存储与屏幕（无需改动）

- 两颗 Flash：程序 Flash（XIP `0x0300C000`）与数据 Flash（25Q128，16 MB，SPI0 `0x140A0000`，CS GPIO `0x12`）。JEDEC 白名单 `src/drivers/flash.c:159-172`，容量固定 `0x1000000`
- 屏幕：`src/drivers/panel.h:35-44` 硬编码 240×320 / ST7789 / BGR565；`panel.c` 无屏型号检测，直接写固定初始化序列。DM36 屏幕相同则整条链可复用

---

## 4. AI 自动化 vs 人工操作分工

### 4.1 AI 可完全自动化（纯代码/数据分析，不碰设备）

| 工作 | 具体产出 |
|---|---|
| 建立板级抽象层 | `board.h` + `board_dm36.h`，收敛所有硬编码引脚/mux，DM32 构建保持等价 |
| GPS 裁剪 | 条件编译 `gps_start()`、GPS 菜单/屏幕，补存根函数 |
| 校准数据解析 | 写脚本按 tag 与 BCD 频点搜索 DM36 备份，输出 DM32/DM36 字段差异报告 |
| 寄存器 dump 解读 | 对比 `fda` dump 与 BK4819 默认值 / `fdc` 缓存，列出差异寄存器与建议值 |
| 编译验证 | 在 `src/` 执行 `make`，修编译错误，检查 `.map` |
| 扩展诊断脚本 | 例如"按住键时自动轮询全部 `GPIO_EXT` 找变化位"，基于 `dm32dbg.py mr` 通道 |
| 文档与清单 | 变更记录、回归测试用例、核对表 |

### 4.2 AI 出命令 + 人工执行物理动作（协作）

| 工作 | AI 负责 | 人工负责 |
|---|---|---|
| Flash 备份/烧录 | 给准确命令、校验输出、判断成败 | 插线、确认 COM 口、**关机→运行→30 秒内开机（不按键）** 时序、决定何时写（`tools/backup/dm32_dump.py:15-17`） |
| 寄存器/GPIO 读取 | 给 `fdr`/`fda`/`mr` 命令序列并解读；经授权也可直接运行 | 连线、开机保持在线、按住 PTT/SK1/SK2 |
| 分层定位按键故障 | 运行 `tools/debug/fkey.py` 向 UI 队列注入 SK1/PTT 事件：若 UI 有反应 ⇒ 输入任务/UI 正常，问题必在 GPIO 层 | 保持连接、观察屏幕反馈 |
| 截图确认 UI | 运行 `tools/debug/fshot.py` 读 framebuffer 生成 PNG 并分析 | 确认当前应显示什么界面 |
| 工具链准备 | 检查安装状态、配 PATH | 玄铁 CDS 注册账号、下载许可证（`README.md:66-69`） |

### 4.3 仍需人工确认

1. **目视/手感确认**：屏幕、背光、按键手感、PTT 防抖；静态截图可由 AI 分析
2. **另一台电台配合**：完成 FM/DMR 定性收发与互通确认
3. **CPS 行为确认**：人工提供可观察的 CPS 表现后，AI 回填机型码 `BOARD_CPS_RADIO_TYPE`

---

## 5. 编排流程

```mermaid
flowchart TD
    A[P0 备份 DM36 两颗 Flash<br/>人工] --> B[P1 解析校准表与寄存器差异<br/>AI]
    B --> C[P2 板级抽象 + GPS 裁剪 + 编译<br/>AI]
    C --> D[P3 连线开机: fda 读 FD6818 寄存器<br/>协作]
    D --> E{SPI 通信是否正常?}
    E -- 否 --> F[核对软 SPI 引脚 0x07/08/18<br/>与 IO_DIPLEX mux]
    F --> D
    E -- 是 --> G[fkey 注入按键事件<br/>UI 有反应?]
    G -- 是 --> H[故障在 GPIO 层<br/>mr 扫真实引脚号]
    G -- 否 --> I[排查输入任务 / UI 层]
    H --> J[填写 board_dm36.h 并重编译]
    I --> J
    J --> K[校准表适配: 移植原厂扇区或改搜索参数]
    K --> L[P4 烧录 + FM 收发实测]
    L --> M{FM 是否通?}
    M -- 否 --> N[查 PA/LNA/音频开关引脚<br/>0x37/0x23/0x22/0x24/0x2F]
    N --> L
    M -- 是 --> O[DMR 实测: 频偏 / 0x50 / 0x43]
    O --> P{是否互通?}
    P -- 否 --> Q[调 4FSK 偏置与寄存器后迭代]
    Q --> O
    P -- 是 --> R[P5 归档: 变更记录 + 回归清单]
```

### P0 — 资料与备份（人工，阻塞后续全部步骤）

| 项 | 内容 |
|---|---|
| 目标 | 保住 DM36 原厂校准数据与可回退镜像 |
| 操作 | `py tools/backup/dm32_dump.py COM6 data backup/dm36/dm36_25q128.bin`（16 MB，约 11 分钟）；再备份 program flash；每颗读两遍比对 |
| 关键约束 | 电台关机 → 启动命令 → **30 秒内开机且不按键**（黑屏、LED 灭）；跑着 OpenDMR 时必须先关机 |
| 交付 | 两份 bin 文件 + DM36 原理图/BOM（若有） |
| 当前条件（2026-11-25） | 用户已有备份文件并可提供 |

### P1 — 离线数据分析（AI）

| 项 | 内容 |
|---|---|
| 输入 | P0 的 bin 文件 |
| 操作 | 扫描数据 Flash 中扇区末字节 `0x02` 且 `0x300/0x328` 首频点落在 UHF/VHF 范围的扇区；解析各字段并与 `DEFAULT_TABLE[]` 对比；检查是否存在 `0x10000` 副本 |
| 交付 | 校准表地址/格式报告；DM32 vs DM36 差异表；是否可直接复用现有搜索逻辑的结论 |
| 验收 | 能明确回答"现有 `factory_cal.c` 能否找到 DM36 的表" |

### P2 — 代码适配（AI）

| 项 | 内容 |
|---|---|
| 操作 | ① 抽 `board.h` / `board_dm36.h`，DM32 保持原值；② 按 P1 结论适配校准搜索/字段；③ 条件编译掉 GPS 菜单、`gps_start()`、存根 GPS 读取；④ `make` 编译并检查 `.map` |
| 交付 | 可编译的 DM36 分支固件；逐项变更清单 |
| 验收 | DM32 构建结果与基线一致；DM36 构建无编译错误 |

### P3 — 分层定位（协作，按此顺序避免盲目改动）

1. **FD6818 是否活着**：`py tools/debug/dm32dbg.py fdr 0x00`、`fda --save dm36.txt`，对照 `docs/fd6818-registers.md` 的 BK4819 复位默认值（0x71=0x8517、0x44=0x9009 等）。全 0 ⇒ 软 SPI 引脚/mux 不对，先查 0x07/0x08/0x18
2. **按键故障分层**：`py tools/debug/fkey.py sk1` / `ptt` 注入 UI 事件。UI 有反应 ⇒ 输入任务与 UI 正常，问题在 GPIO 层；无反应 ⇒ 查 `input.c`/UI
3. **定位真实引脚**：人工按住 PTT/SK1/SK2，AI 用 `mr` 读 `GPIO_EXT`。地址算法（`src/hal/hw.h:65-71`）：pin 32–63 时 `GPIO_EXT = 0x14100050`，PTT(43)=bit11、SK1(51)=bit19、SK2(52)=bit20
4. **回写配置**：把实测引脚号填入 `board_dm36.h`，重新编译

### P4 — 实机烧录与射频验证（人工执行，AI 调参）

| 阶段 | 判定点 | 失败时排查 |
|---|---|---|
| 烧录 | `fw_update.py` 增量升级或 `dm32_dump.py --write` | 确认型号头、起始地址 0xC000 |
| FM 收发 | 用另一台电台做定性收听/发射：能听到信号、对方能收到 | 查软件寄存器/板级宏：PA/LNA 引脚 0x37/0x23/0x22/0x24、音频开关 0x2F、`0x47` 音频输出位 |
| DMR 收发 | 与另一台电台定性互通、能解码 | 4FSK 偏置（校准表）、`0x50=0xBB20` 基带位、`0x43=0x4048` 带宽、`hr_c7000` 同步表 |
| 回归 | 扫描、双守、联系人、字库 | 存储布局与 DM32 相同，理论上全部可用 |

### P5 — 归档（AI + 人工确认）

更新本文档的"实测结果"章节；提交 `board_dm36.h` 与变更清单；记录 DM36 专属校准地址与寄存器差异；列出仍未解决项。

---

## 6. 核对清单

### 按键

- [ ] PTT 实际 GPIO（DM32 = 43）
- [ ] SK1 / SK2 实际 GPIO（DM32 = 51 / 52）
- [ ] 矩阵行是否仍复用 LCD DB0–DB4；若是，`IO_DIPLEX2` 掩码值
- [ ] `IO_DIPLEX0/1/2`、GPIOA/B/C DDR/DR、上下拉初值
- [ ] 旋钮 A/B 引脚与 TIMER2 轮询
- [ ] 键盘背光 `0x28`、LED 32/33
- [ ] `UI_KEY_*` 与 `KEYS_*` 位号对齐（若键位数量变化）

### 射频

- [ ] FD6818 软 SPI：SCL 0x07 / SDA 0x08 / SCN 0x18 + mux
- [ ] GP0/GP1 0x3B / 0x3A
- [ ] PA/LNA：0x37 / 0x23 / 0x22 / 0x24；MIC_MUT 0x2E；AUDIO_SW 0x2F；AUDIO_AMP 0x35
- [ ] 校准扇区 tag、地址范围、字段偏移、频率 BCD 格式
- [ ] 0x50 基带位、0x43 带宽、0x47 音频输出、0x73 AFC
- [ ] `modem.c` 的 `RF_MODE` / `RF_CONTROL` / `SIG_CENTER` / DAC 偏置（最后再动）

### 其他

- [ ] GPS 裁剪完整（菜单、屏幕、`gps_start()`、存根）
- [ ] 型号字符串与镜像头（`firmware_info.c`、`cable_link.c`、`Makefile`、`mkboot.py`）
- [ ] 程序 Flash 起始地址 `0x0300C000` 与升级头
- [ ] 数据 Flash JEDEC ID 白名单与容量
- [ ] 电池 ADC 系数 `ADC_PER_VOLT_X100 = 82`（`src/services/battery.c:45`，标注为 DM-32）

---

## 7. 调试命令速查

| 用途 | 命令 |
|---|---|
| 构建 DM32（默认） | `cd src; make` → `build/opendmr.bin` |
| 构建 DM36 | `cd src; make BOARD=dm36` → `build/opendmr_dm36.bin` |
| 备份数据 Flash | `py tools/backup/dm32_dump.py COM6 data backup/dm36/dm36_25q128.bin` |
| 烧录固件 | `py tools/backup/dm32_dump.py COM6 program src/build/opendmr.bin --write --start 0xC000` |
| 增量升级 | `py tools/debug/fw_update.py src/build/opendmr.bin --port COM6` |
| 读 FD6818 寄存器 | `py tools/debug/dm32dbg.py fdr 0x0c` |
| 全量 dump FD6818 | `py tools/debug/dm32dbg.py fda --first 0 --count 0x80 --save dm36.txt` |
| 对比寄存器缓存 | `py tools/debug/dm32dbg.py fdcdiff dm32cache.txt dm36.txt` |
| 读内存/GPIO | `py tools/debug/dm32dbg.py mr 0x14100050 4 --width 4` |
| 注入按键 | `py tools/debug/fkey.py sk1`（支持 `sk2`/`ptt`/`rot:3`/`long:green`） |
| 截图 | `py tools/debug/fshot.py shot.png --scale 2` |
| 机型信息 | `py tools/debug/dm32dbg.py info` |

---

## 8. 实测结果记录（待填）

| 阶段 | 日期 | 结果 | 备注 |
|---|---|---|---|
| P0 备份 | 2026-11-25 | 已提供并解析 | data 16 MB、program 1 MB；program 版本 `DM36.NRFB.01.026` |
| P1 校准解析 | 2026-11-26 | 通过 | 找到原厂校准扇区 `0xA2000`；`factory_cal.c` 搜索规则可直接命中 |
| P2 编译 | 2026-11-25 | 通过 | DM36 `BUILD_EXIT=0`；工具链与目录见 9.4/9.6 |
| P3 FD6818 通信 | — | 待软件 dump | 采集 `fda`/`fdcdiff` |
| P3 按键引脚 | 2026-11-26 | 基本解决 | 用户确认按键问题基本解决；最终引脚记录待归档 |
| 按键音破音 | 2026-11-26 | 已修复，待实机确认 | DM36 静音 FD6818B AF 输出 + 延长功放启动等待，见 10.6 |
| FD6818B 原厂表 | 2026-11-26 | 已提取并应用 | 原厂 PLL/VCO 表与 UV-K1 BK4829 表差异大，见 10.7 |
| 蓝牙模块 | 2026-11-26 | 已提取协议，基础驱动完成 | LytLink AT 命令 + UART2 115200，见 10.8；音频/写频待实机串联 |
| P4 FM 收发 | — | 待定性测试 | 另一台电台定性收发 |
| P4 DMR 互通 | — | 待定性测试 | 另一台电台定性互通 |
| P5 归档 | — | 持续更新 | 本文即剩余工作记录 |

---

## 9. 已完成的代码层适配（2026-11-25）

### 9.1 设计原则

- **DM32 是默认**：不传 `BOARD` 时构建结果与改造前逐值等价
- **DM36 全在 `src/board/`**：型号映射、差异覆盖、无 GPS 存根都不混入业务目录
- **上游冲突最小化**：共享文件每处只做一行"字面值 → 宏"替换，共 13 个文件 +82/-66 行

### 9.2 新增文件

| 文件 | 作用 |
|---|---|
| `src/board/board.h` | 按 `BOARD_DM36` 选择映射；`hw.h` 自动包含它 |
| `src/board/board_dm32.h` | 60 个板级宏，原样转录原字面值 |
| `src/board/board_dm36.h` | `#include "board_dm32.h"` + 覆盖 `BOARD_NAME="DM-36"`、`BOARD_HAS_GPS=0`；待实测项集中在 `TODO(DM36)` 注释里 |
| `src/board/gps_stub.c` | DM36 下替代 `services/gps.c`，5 个 GPS API 空实现 |

### 9.3 已改造的共享文件（仅宏替换）

`hw.h`（引脚）、`gpio.c`（mux/DDR/DR 初值，GPS 电源脚按机型裁剪）、`keys.c`（列/PTT/矩阵）、`rotary.c`、`leds.c`、`fd6818.c`（软 SPI）、`radio_internal.h`（PA/LNA/频段）、`gps.c`、`rda5807.c`、`battery.c`、`firmware_info.c`、`cable_link.c`、`Makefile`。

### 9.4 构建方式

已验证的工具链放在项目内：

- 编译器：`toolchain/MinGW/csky-elf-noneabiv2-tools/bin`
- make：`toolchain/MinGW/msys/1.0/bin`

在仓库根目录执行：

```powershell
$env:PATH = "E:\peng\OpenDMR\toolchain\MinGW\csky-elf-noneabiv2-tools\bin;E:\peng\OpenDMR\toolchain\MinGW\msys\1.0\bin;$env:PATH"
cd src
make clean
make CROSS=csky-elf-noneabiv2-            # DM32 → build/opendmr.bin
make CROSS=csky-elf-noneabiv2- BOARD=36   # DM36 → build/opendmr_dm36.bin（BOARD=dm36 亦可）
```

`CROSS=csky-elf-noneabiv2-` 必须显式传入：Makefile 默认值仍是上游的 `csky-elfabiv2-`，那套是 Minilibc，缺少 `reent.h`，不能编译当前固件。

DM36 构建从源列表剔除 `services/gps.c`、加入 `board/gps_stub.c`；不编译 GPS 服务、不注册 UART1、不触碰 GPS 电源脚。GPS 菜单和屏幕仍可显示但只读、无副作用，无需改 UI 代码。

### 9.5 验证状态

| 检查 | 结果 |
|---|---|
| 宏定义完整性 | 59 个引用全部有定义（未使用项仅 include guard） |
| 裸引脚字面值残留 | `grep '^#define PIN_.* 0x?[0-9]'` 无命中 |
| 裸 `gpio_output(0x..)` 残留 | 无命中 |
| DM32 行为等价 | 逐值核对通过，未改逻辑 |
| Git 子模块 | 已执行 `git submodule update --init`，`vocoder`、`p25` 已拉取 |
| **实际编译** | **通过**：使用 C-SKY V3.10.30 Newlib abiv2 / GCC 6.3.0（`csky-elf-noneabiv2-gcc`）完整编译 DM36 成功，`BUILD_EXIT=0` |
| **源码问题** | 已修复 `src/board/board_dm36.h:46` 注释中的 `*/` 提前闭合问题 |
| **实机功能** | **未执行**：待 P0 备份与 P3 测量 |

### 9.6 编译环境问题与工具链下载（2026-11-25）

#### 问题现象

- `README.md:66` 写的是“玄铁 Xuantie-800 GCC 13.3.0（csky-elfabiv2）”，但公开下载的 CDS 5.2.21 / 5.2.32 中，`csky-elfabiv2-tools` 实际是 **Minilibc/6.3.0**。
- 该工具链没有 `reent.h`，编译 `src/hal/newlib.c` 时报 `fatal error: reent.h: No such file or directory`。
- `csky-elfabiv2-*` 是 Minilibc 前缀；C-SKY 800 带完整 newlib 的裸机工具链前缀是 `csky-elf-noneabiv2-`。

#### 调查结论

- 玄铁 CPU 软件开发指南 V3.3 明确写明 CSKY 系列工具链发布版本基于 **GCC 6.3**。
- 未找到公开可下载的 `csky-noneabiv2` / `csky-elf-noneabiv2` **GCC 13.3.0 + newlib** 官方包；README 的 GCC 13.3.0 描述与当前公开 C-SKY 800 工具链不符。
- 实测 `C-SKY V3.10.30 Newlib abiv2 / GCC 6.3.0` 含 `reent.h`、支持 `-mcpu=ck803s`，可以完整编译 DM36。

#### 下载链接

| 资源 | 地址 |
|---|---|
| 玄铁 GCC-800 工具链 V3.10.30 下载页（需登录） | https://www.xrvm.cn/community/download?id=4224178809689149440 |
| 官方 V3.10.30 Release Note（包名与 MD5） | https://occ-oss-prod.oss-cn-hangzhou.aliyuncs.com/resource//1678185528238/ReleaseNote.pdf |
| 玄铁 CPU 软件开发指南 V3.3 | https://occ-oss-prod.oss-cn-hangzhou.aliyuncs.com/resource//1721094254407/%E7%8E%84%E9%93%81CPU%E8%BD%AF%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97V3.3.pdf |
| CDS V5.2.21 软件包下载页（README 原链接，msys/make 来源） | https://www.xrvm.cn/community/download?versionId=4498138066258563072 |
| GitHub 直链镜像（旧版 Windows newlib 包，可选） | https://github.com/crosstyan/Xuantie-800-gcc-elf-newlib-prebuilt/releases/download/v0.00/csky-elf-noneabiv2-tools-mingw-newlib-20210423.tar.gz |

#### 推荐包与本地目录

在 V3.10.30 下载页选择：

```text
csky-elf-noneabiv2-tools-mingw-newlib-20230301.tar.gz
MD5: 052e4f556f6a56048258fbfb8ae4c234
```

已复制到项目内：

```text
toolchain/MinGW/csky-elf-noneabiv2-tools/bin
toolchain/MinGW/msys/1.0/bin
```

验证命令与结果：

```powershell
& toolchain\MinGW\csky-elf-noneabiv2-tools\bin\csky-elf-noneabiv2-gcc.exe --version
& toolchain\MinGW\msys\1.0\bin\make.exe --version
```

```text
T-HEAD C-SKY Tools V3.10.30 Newlib abiv2 B20230301 6.3.0
GNU Make 3.81
BUILD_EXIT=0
text 728512 / data 3444 / bss 321908
```

Git 子模块已初始化（2026-11-25 执行 `git submodule update --init` 成功）。

### 9.7 下一步（需实测/数据后回填）

1. 依据 P3 `mr` 实测结果，把引脚号填入 `board_dm36.h` 对应宏
2. 若 FD6818 校准表格式不同，修改 `factory_cal.c`——依赖 P1 备份解析结果
3. 确认 DM36 升级头：已有备份 bin，待离线分析后确认；当前沿用 `BFUV32-V2`
4. 确认 CPS 机型码 `BOARD_CPS_RADIO_TYPE`：待人工观察到 CPS 行为后填写；当前沿用 11

---

## 10. 剩余工作记录（持续更新）

> 从本节起，本文作为 DM36 剩余工作的主记录；后续进展只更新本节与“8. 实测结果记录”。

### 10.1 当前可用资源

| 项 | 状态 |
|---|---|
| DM36 Flash 备份 | 已提供并完成解析；data/program 两个 bin |
| 授权许可 | 用户已说明具备 |
| 软件调试通道 | 可用时走 `fdr`/`fda`/`fdcdiff`/`mr`/`fkey`/`fshot` |
| 射频互通验证 | 另一台电台定性收发/互通 |
| 编译环境 | 已就绪并完成 DM36 构建，见 9.4/9.6 |

### 10.2 剩余任务

| 编号 | 任务 | 负责 | 输入 / 前置 | 状态 | 下一步 |
|---|---|---|---|---|---|
| R1 | 校验/解析 DM36 Flash 备份 | AI + 人工提供 | 备份 bin | 完成 | 已解析 data/program；校准扇区见 10.3 |
| R2 | P1 校准表解析 | AI | R1 | 完成 | 已定位 `0xA2000`；`factory_cal.c` 搜索逻辑可直接命中 |
| R3 | FD6818B 软件 dump 解读 | AI | `fda`/`fdcdiff` 输出 | 待采集 | 人工连接串口后采集；先确认芯片 ID `0x4829`，AI 按 BK4829 表分析 |
| R4 | 按键 GPIO 定位 | 协作 | 串口 + 人工按键 | 基本解决 | 用户确认按键问题基本解决；`*` / `#` 映射待刷入新固件验证 |
| R5 | 回填 `board_dm36.h` | AI | R3/R4 结果 | 待最终记录 | 已回填实测矩阵映射；最终按键行为以新固件验证结果归档 |
| R6 | `factory_cal.c` 适配 | AI | R2 结果 | 暂不改动 | 当前备份中原厂表有效，搜索/字段布局匹配 |
| R7 | 定性收发/互通测试 | 人工执行 + AI 调参 | R5/R6 固件 + 另一台电台 | 待执行 | 完成可听/可解码/可互通判断 |
| R8 | 文档/清单归档 | AI | 以上结果 | 持续 | 更新本节与“8. 实测结果记录” |

### 10.3 DM36 校准信息分析（2026-11-26）

输入文件：

| 文件 | 大小 | MD5 |
|---|---:|---|
| `dm36-data-20261125-1940.bin` | `0x1000000` | `032e1b483f0367028591ce7523672eac` |
| `dm36-program-20261125-1933.bin` | `0x100000` | `fff0e0eb0638299c5b448b33b2d0e5da` |

程序 Flash 中识别到原厂版本串：

```text
DM36.NRFB.01.026
2022-06-27
D1.01.01.004 / S1.00.01.001 / C1.00.01.001 / R1.00.01.001
```

版本串位于 program Flash 偏移 `0xB0FBC`。

数据 Flash 中共发现 6 个末字节为 `0x02` 的扇区：

```text
0xA2000, 0x105000, 0x116000, 0x218000, 0x24A000, 0x25D000
```

按 `factory_cal.c:39-55` 的筛选条件（末字节 `0x02`，`0x300`/`0x328` 首频点分别落在 UHF/VHF 范围）只有 `0xA2000` 通过，判定为 DM36 原厂校准扇区。

OpenDMR 的校准副本位置 `0x710000` 当前全为 `0xFF`。首次启动 OpenDMR 时会把 `0xA2000` 原样复制到该位置，并在菜单显示原厂表地址。

关键字段解析：

| 字段 | 偏移 | DM36 实测值 | `DEFAULT_TABLE[]` | 结论 |
|---|---|---|---|---|
| TX Freq Adjust | `0x016` | `113` | `110` | 原厂表值不同，直接读取 |
| UHF 4FSK bias | `0x017` | `80 78 71 69 66` | `77 74 71 68 66` | 原厂表值不同，影响 DMR/UHF 4FSK 偏置 |
| RX Freq Adjust | `0x027` | `497` | `494` | 原厂表值不同，直接读取 |
| UHF power | `0x040` | `105 100 87 89 86 / 142 123 134 134 116 / 193 201 207 190 181` | 与缺省表不同 | 原厂表值不同，影响 UHF PA DAC 插值 |
| VHF 4FSK bias | `0x0A6` | `236 219 208 197 177` | `226 213 199 188 176` | 原厂表值不同，影响 DMR/VHF 4FSK 偏置 |
| VHF power | `0x0D0` | `93 78 93 93 86 / 105 127 139 142 133 / 200 194 212 227 225` | 与缺省表不同 | 原厂表值不同，影响 VHF PA DAC 插值 |
| Analog/Digital RX Level | `0x20A` | `6 / 36` | `13 / 39` | 原厂表值不同，影响 FM 接收音量与 DMR 接收 DAC 增益 |
| UHF 校准频点 | `0x300` | `400 / 417.5 / 435.5 / 452.5 / 470 MHz` | 第三点为 `435 MHz` | 原厂表值不同；使用原厂表时按实际频点插值 |
| VHF 校准频点 | `0x328` | `136 / 145.5 / 155.5 / 164.5 / 174 MHz` | 第三点为 `155 MHz` | 原厂表值不同；使用原厂表时按实际频点插值 |
| 来源标记 | `0xFFE` | `0xFF` | — | 原厂原样数据 |
| 扇区标记 | `0xFFF` | `0x02` | `0x02` | 搜索条件匹配 |

解析结论：

1. `factory_cal.c` 的搜索范围 `0x1000..0xC9000` 包含 `0xA2000`；`is_table()` 的 tag 检查和 `read_sector()` 的频率/字段检查都通过。
2. 当前不需要修改 `factory_cal.c` 的搜索逻辑或字段偏移；DM36 原厂表格式与 DM32 现有解析一致。
3. DM36 的功率、4FSK 偏置、RX level 与 `DEFAULT_TABLE[]` 有明显差异，射频调试应优先使用原厂扇区 `0xA2000`，保持 data Flash 原样。
4. 缺省表只作为备份路径；当前备份已有有效原厂表，R6 暂不改动。

### 10.4 下一步射频采集命令（R3）

校准解析完成后，射频任务进入 FD6818 软件 dump。连接设备后采集：

```powershell
py tools/debug/dm32dbg.py --port COM6 fdr 0x00
py tools/debug/dm32dbg.py --port COM6 fda --save dm36.txt
py tools/debug/dm32dbg.py --port COM6 fdcdiff dm32cache.txt dm36.txt
```

采集结果用于：

- `fdr 0x00` 应读到 `0x4829`（DM36 的 FD6818B / BK4829）；DM32 才是 `0x4819`（FD6818 / BK4819）
- 对照 `docs/fd6818-registers.md` 的 BK4829 默认值，不直接套用 DM32 的 BK4819 表
- 判断 FD6818B 软 SPI 是否正常
- 判断是否调整 `board_dm36.h` 中的 FD6818B、PA/LNA、音频开关引脚
- 为 P4 的 FM/DMR 定性收发测试提供调参依据

### 10.5 星键 / 井键映射验证（2026-11-26）

用户反馈：当前机器上 `#` 可以切换配置项，`*` 没有反应。按 DM32 的 P1/P2 使用方式，期望为：

| 键 | 行为 |
|---|---|
| `*`（P1） | 上一个 / 减小 |
| `#`（P2） | 下一个 / 增大 |

当前源码已经按此实现：

- `src/board/board_dm36.h`：矩阵位置 `0` 映射到 `KEYS_STAR`
- `src/ui/list.c`：`BOARD_DM36` 下 `UI_KEY_STAR` → `change_value(..., -1)`，`UI_KEY_HASH` → `change_value(..., +1)`

发现当时机器中的固件早于上述两处源码改动：

```text
src/ui/list.c          2026-11-26 08:55
src/board/board_dm36.h 2026-11-26 08:55
旧 opendmr_dm36.bin    2026-11-25 14:17
```

已重新编译 DM36 固件：

```text
src/build/opendmr_dm36.bin
BUILD_EXIT=0
2026-11-26 09:28
```

新固件需刷入后再验证 `*` / `#`。若验证时 `#` 仍能增大、`*` 仍无反应，下一步用 `fkey.py star` 注入 UI 事件区分“物理键映射”和“UI 改值逻辑”：

```powershell
py tools/debug/fkey.py star
py tools/debug/fkey.py hash
```

### 10.6 DM36 按键音“破音”修复（2026-11-26）

**现象**：DM36 固件下按键音听起来失真/发破，DM32 固件同链路正常。

**提示音链路**（两机共用）：

```text
ui.c -> sound.c: SOUND_KEY {600 Hz, 60 ms}
     -> note(): 2048 点正弦表，按 beep_volume_divider 右移（默认 4，约 -24 dBFS）
     -> voice_play_beep_put(): 16-sample 块环形缓冲（8 块 = 16 ms）
     -> voice_play.c feed_task: 1 kHz 写一块进 HR_C7000 提示音 FIFO (0x16000980)
     -> radio_beep_path(): 音频开关 0x2F 拉低，切到基带提示音
     -> radio_beep_amp(): 功放 0x35 打开，等 AMP_START_MS 后开始送音
```

**原因定位**：

1. DM36 与 DM32 的提示音**生成和 FIFO 写入部分完全相同**，波形、音量、采样率都不随机型改变；原厂 DM36 程序同样使用 `0x16000980` 提示音 FIFO，因此提示音通道本身没问题。
2. DM36 仍继承 DM32 的音频板级宏：
   ```c
   BOARD_PIN_AUDIO_SW   = 0x2F
   BOARD_PIN_AUDIO_AMP  = 0x35
   ```
   而 DM36 的射频是 **FD6818B / BK4829**，初始化表按芯片 ID `0x4829` 自动切换（`fd6818.c`）。原厂 DM36 固件在音频模式切换时成对写 FD6818 `0x2E/0x2F`（`0x8608/0x1890` 与 `0x9608/0x9890`），OpenDMR 驱动只写 `0x2F` 且仅用于 AM，DM36 上 FD6818B 的模拟 AF 输出没有被显式静音；它与基带提示音叠加后就是听到的“破音”。
3. `voice_play.c` 的放大器启动等待是 DM32 调出的 `75 ms`。DM36 的功放启动更慢，而 `SOUND_KEY` 本身只有 `60 ms`，起始失真会覆盖整声按键音。

**修复**（均为 DM36 条件编译，DM32 构建逐值等价）：

| 文件 | 改动 |
|---|---|
| `src/services/radio_rx.c` | `radio_beep_path()` 在 `BOARD_DM36` 下调用 `fd6818_af_out(!on && rs.has_carrier)`：提示音期间静音 FD6818B 的 AF 输出，结束后按 squelch 状态恢复；与 `radio_broadcast_amp()` 的已有做法一致。 |
| `src/services/voice_play.c` | `BOARD_DM36` 下 `AMP_START_MS` 由 `75` 提高到 `120`，给 DM36 功放更多启动时间；新增 `board.h` 包含以取得板级宏。 |

**编译验证**：

```text
make clean; make CROSS=csky-elf-noneabiv2- BOARD=36
BUILD_EXIT=0
text 731184 / data 3444 / bss 322404
src/build/opendmr_dm36.bin
```

**实机验证待做**：刷入新固件后按任意键听按键音；若仍有残留失真，下一层排查用 `fdr 0x47/0x2E/0x2F` 与 `mr` 读 `GPIO 0x2F/0x35` 在放音前后的变化，并按原厂值补齐 FD6818B 的 `0x2E/0x2F` 写入。

### 10.7 原厂 FD6818B 寄存器表与 DM36 收发修复（2026-11-26）

从原厂程序 Flash `DM36.NRFB.01.026` 反汇编中提取了完整的 FD6818B 初始化序列（代码位于 `0x0300ed96`，经 `bsr` 调用统一写寄存器函数），并与 OpenDMR 当前 `init_settings_b`（来自 UV-K1/K5v3 的 BK4829 驱动）逐项对比。

**提取方法**：objdump 反汇编原厂 bin（`--adjust-vma=0x03000000`），搜索 `movi r0,<reg>; movi r1,<value>; bsr <write_fn>` 模式；写寄存器函数被 231 处调用，覆盖寄存器 `0x00..0x7E`。带 `lsli` 的值已按位移还原（如 `0x10 = 198<<2 = 0x0318`）。

**关键差异表**（原厂 DM36 vs OpenDMR `init_settings_b`）：

| 寄存器 | 原厂 DM36 | OpenDMR (UV-K1 BK4829) | 作用 |
|---|---|---|---|
| `0x1C` | `0x0422` | `0x07C0` | PLL / 参考分频 |
| `0x1D` | `0x2AAB` | `0xE555` | PLL / VCO |
| `0x1F` | `0x165A` | `0xC65A` | VCO 偏置（bit15 差异致命） |
| `0x25` | `0x6DBA` | `0xC1BA` | VCO / 电荷泵 |
| `0x28` | `0x0A40` | `0x0B40` | 校准相关 |
| `0x2C` | `0x0022` | `0x1822` | 接收链路 |
| `0x2F` | `0x9890` | 未写（只读默认值） | 音频路由 |
| `0x3A` | `0x9A7C` | 未写 | — |
| `0x3F` | `0x07FE` | 未写 | — |
| `0x40` | `0x34F0` | 未写 | 频段相关 |
| `0x46` | `0x6050` | `0x600A` | 音频 |
| `0x48` | `0xB386` | `0x33EF` / `0xBBBF` | AGC / 音量 |
| `0x49` | `0x2A32` | `0x2AB2` | AGC 阈值 |
| `0x4C` | `0xE520` | 未写 | — |
| `0x4D` | `0xA015` | 未写 | — |
| `0x4E` | `0x1F10` | `0x4708` | — |
| `0x4F` | `0x2A28` | `0x302F` | — |
| `0x73` | `0x6681` | 未写（AFC 由代码置位） | AFC |
| `0x7E` | `0x303E` | `0x302E` | AGC 模式 |

DMR 模式表（`0x0300f26e`）同样不同：原厂写 `0x43=0x0048`（OpenDMR 为 `0x4048`）、`0x47=0x6740/0x6943`（OpenDMR 为 `0x0140`）、`0x7E=0xB03F`（OpenDMR 为 `0x303F`）。

**结论**：DM36 无法收发的根本原因是 **OpenDMR 的 FD6818B 表不是 DM36 的值**。PLL/VCO 寄存器（`0x1C/0x1D/0x1F/0x25`）错误会使频率合成器无法锁定，表现为完全收不到也发不出。此外 `fd6818_init()` 依赖读寄存器 `0x00` 判断芯片型号，而原厂固件复位后直接写表、从不读 ID——ID 识别若失败会进一步选错成 FD6818（DM32）表。

**修复**（`src/drivers/fd6818.c`，全部 `#if defined(BOARD_DM36)`，DM32 行为不变）：

| 项 | 改动 |
|---|---|
| `init_settings_dm36[]` | 新增，逐值照抄原厂初始化序列 |
| `post_init_settings_dm36[]` / `fm_settings_dm36[]` | 新增，沿用原厂 `0x37=0x9D1F`、`0x48=0xB386` |
| `dmr_settings_dm36[]` | 新增，使用原厂 `0x43=0x0048`、`0x47=0x6740/0x6943`、`0x7E=0xB03F` |
| `fd6818_dm36` | 新增芯片配置结构，AGC / PA 参数按原厂表 |
| `fd6818_init()` | DM36 构建强制选用 `fd6818_dm36`，不依赖 ID 读取 |
| `BANDWIDTH_DMR` | DM36 改为 `0x0048` |

**编译验证**：

```text
make clean; make CROSS=csky-elf-noneabiv2- BOARD=36
BUILD_EXIT=0
text 731072 / data 3444 / bss 322404
```

**实机验证步骤**：
1. 刷入新固件，`fdr 0x00` 期望读到 `0x4829`（DM36 = FD6818B）。
2. 若读不到，先查软 SPI 引脚/mux（当前继承 DM32 的 `SCL=0x07 / SDA=0x08 / SCN=0x18`）。
3. 能读到 ID 后，用另一台电台做 FM 定性收发；再测 DMR 互通。
4. 若仍无收发，用 `fda` 导出全部 FD6818 寄存器，与本文原厂表比对。

### 10.8 蓝牙模块（LytLink）与实现（2026-11-26）

**结论**：DM36 的蓝牙不是 SoC 内置，而是**外接蓝牙 PTT/音频模块**，原厂固件通过私有 AT 命令控制；固件中没有独立的蓝牙芯片型号串，模块品牌/协议名为 **LytLink**（菜单项 `LytLink`、`LytLink Ver:`、`LytLink Not Open!`）。具体芯片型号需实物模块丝印确认；对移植来说关键是接口与协议，而不是芯片 die。

**硬件接口**（从原厂固件反汇编确认）：
- UART：**UART2 @ `0x14090000`**，115200 8N1。证据：蓝牙字节输出函数 `0x03061a2c` 用 `movih r2,5129`（= `0x1409`），轮询 `(r2+0x7c)&2`（USR TX FIFO 满），写 `(r3+0)`（THR）；UART2 初始化 `0x301c990` 的分频是 `(clock_apb>>4)/115200`（`225<<9`）。
- DM32 只有 UART0（编程线 `0x14030000`）/ UART1（GPS `0x14040000`），没有 UART2 用途，所以 DM36 才引入该驱动。
- 模块电源/复位：原厂蓝牙初始化 `0x030619ec` 附近按状态操作 GPIO `350`（`175<<1`）与 `48`；这些脚在未实测前不改 `board_dm36.h`，沿用原厂 `pins_init()` 的 mux/DR 初值。

**AT 协议**（字符串 `0x030ba948`-`0x030bab90`）：

| 方向 | 命令/响应 |
|---|---|
| 版本/状态 | `AT+VER?` → `VER:`；`AT+AUTOCONN?`；`READY` |
| 配对/连接 | `AT+LENAME=`/`?`、`AT+PIN=`/`?`、`AT+DNAME?`、`AT+CONN=<addr>`、`AT+DISC=<addr>`、`AT+SCAN=1/2`、`AT+LESCAN=1/2`、`SCAN:001122334455`、`CONN`、`DISC`、`HOSTDEVICE` |
| 音频 | `AT+AGHF=0/1`（HFP）、`AT+MICEN=0/1`、`AT+SPKEN=0/1`、`AT+AUDIO=0/1`、`AT+MICGAIN=15`、`AT+SPKGAIN=25`、`HFREQ` |
| PTT | `PTTKEY`、`PTTBATT`、`3900mV` … `3300mV` |
| 写频/透传 | `AT+DATA=00,<n>`、`AT+ADATA=01,<n>`、`+DATA:00`、`+ADATA:00` |

**实现**（全部 DM36 条件编译，DM32 构建逐值等价）：
- `src/drivers/uart.c/.h`：新增 UART2 驱动（`uart2_init/uart2_write/uart2_read`，115200 8N1，轮询收发，无中断）。
- `src/services/bluetooth.c/.h`：新增蓝牙服务，启动时发 `AT+VER?` / `AT+AUTOCONN?`，解析 `CONN`/`DISC` 维护链路状态；提供 `bluetooth_audio()`（AGHF/SPKEN/MICEN/AUDIO）与 `bluetooth_write_data()`（AT+DATA 透传）。
- `src/app/main.c`：DM36 下 `bluetooth_start()`。

**编译验证**：DM32 与 DM36 均 `BUILD_EXIT=0`；DM36 固件已重新生成。

**仍待实机/后续**：
1. 蓝牙音频最终要经过音频开关/功放路由到蓝牙模块的模拟音频脚；当前服务只完成模块的 HFP/SPK/MIC 控制，音频开关的具体 GPIO 需实测确认后接入。
2. 蓝牙写频需把 `+DATA:` 帧接到 `cable_link`（CPS 协议）的输入/输出；当前仅实现 UART2 透传帧的收发接口 `bluetooth_write_data()`。
3. 用串口抓 UART2（115200）确认 `AT+VER?` 的 `VER:` 响应，从而得到 LytLink 固件版本/模块型号。

---

## 11. 按键对应检测

按键对应按“两层定位”：先确认 UI/输入任务，再读 GPIO 实际状态。

### 11.1 UI 层验证

```powershell
py tools/debug/fkey.py sk1
py tools/debug/fkey.py sk2
py tools/debug/fkey.py ptt
```

判定：

- 屏幕有按键反应：UI 与输入任务正常，继续 GPIO 定位
- 屏幕无反应：先查 `input.c` / UI 层

### 11.2 GPIO_EXT 读取

地址算法来自 `src/hal/hw.h:66-72`：

```text
GPIO_EXT(pin) = GPIO_BASE(pin) + 0x50
```

| Bank | pin 范围 | `GPIO_EXT` 地址 |
|---|---|---|
| GPIO A | 0–31 | `0x14020050` |
| GPIO B | 32–63 | `0x14100050` |
| GPIO C | 64–95 | `0x14110050` |

读取命令，假设串口为 `COM6`：

```powershell
py tools/debug/dm32dbg.py --port COM6 mr 0x14020050 4 --width 4
py tools/debug/dm32dbg.py --port COM6 mr 0x14100050 4 --width 4
py tools/debug/dm32dbg.py --port COM6 mr 0x14110050 4 --width 4
```

### 11.3 采样与换算

对每个按键：

1. 不按键，连续读 3 次，记为空闲值
2. 人工按住 PTT / SK1 / SK2，连续读 3 次
3. 松开，再读 3 次
4. 比较按住时变化的 bit

换算公式：

```text
pin = bank 起始 pin + bit 序号
```

读 `0x14100050` 时属于 GPIO B，起始 pin 为 32。DM32 参考值：

| 按键 | 变化 bit | 换算 | pin |
|---|---|---|---|
| PTT | bit11 | 32 + 11 | 43 |
| SK1 | bit19 | 32 + 19 | 51 |
| SK2 | bit20 | 32 + 20 | 52 |

DM36 以实测结果为准。例如按住 PTT 时 `0x14100050` 的 bit13 变化，则：

```text
PTT = 32 + 13 = 45
```

### 11.4 矩阵键

PTT、SK1、SK2 通常是直接 GPIO。

数字键、`*`、`#`、方向键可能走矩阵。矩阵键按住时可能同时看到两个 bit 变化：一个行、一个列。这类按键需要同时对照：

- `src/hal/keys.c`
- `IO_DIPLEX0/1/2`
- `GPIOA/B/C DDR/DR`
- `src/board/board_dm32.h` / `src/board/board_dm36.h`

### 11.5 回填配置

拿到有实测证据的引脚号后，回填 `src/board/board_dm36.h` 对应宏，再按 9.4 的命令重新编译 DM36 固件。

---

## 12. Ubuntu 26.04 开发环境安装

### 12.1 安装系统开发包

仅编译固件：

```bash
sudo apt update
sudo apt install -y build-essential git
```

编译并使用 `tools/` 下的调试、备份、截图脚本：

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  git \
  python3 \
  python3-pip \
  python3-serial \
  python3-pil \
  python3-numpy
```

| 包 | 用途 |
|---|---|
| `build-essential` | `make`、GCC、binutils |
| `git` | 子模块与版本信息 |
| `python3` | 所有 `tools/*.py` |
| `python3-serial` | `dm32dbg.py`、`dm32_dump.py`、`fw_update.py` |
| `python3-pil` | `fshot.py`、`mkboot.py`、`mkfont.py` |
| `python3-numpy` | `miclevel.py` |

串口权限：

```bash
sudo usermod -aG dialout $USER
```

执行后重新登录。串口设备通常是 `/dev/ttyUSB0` 或 `/dev/ttyACM0`。

### 12.2 安装 Linux x86_64 C-SKY newlib 工具链

下载页：

```text
https://www.xrvm.cn/community/download?id=4224178809689149440
```

选择包：

```text
csky-elf-noneabiv2-tools-x86_64-newlib-20230301.tar.gz
MD5: 93f5f03c725d09f6f590a9380870e844
```

校验下载文件：

```bash
md5sum csky-elf-noneabiv2-tools-x86_64-newlib-20230301.tar.gz
```

解压与 PATH：

```bash
mkdir -p ~/opt/csky
tar -xzf csky-elf-noneabiv2-tools-x86_64-newlib-20230301.tar.gz -C ~/opt/csky
export PATH="$HOME/opt/csky/csky-elf-noneabiv2-tools/bin:$PATH"
```

若解压后的目录名不同，以包含 `bin/csky-elf-noneabiv2-gcc` 的实际目录为准。

验证：

```bash
csky-elf-noneabiv2-gcc --version
csky-elf-noneabiv2-gcc -dumpmachine
```

### 12.3 编译 DM36

Ubuntu 下直接使用系统 `make`，无需额外 make 工具：

```bash
cd src
make clean
make CROSS=csky-elf-noneabiv2- BOARD=36
```

### 12.4 调试命令

Ubuntu 下使用 `python3`，串口使用 Linux 设备名：

```bash
python3 tools/debug/dm32dbg.py --port /dev/ttyUSB0 mr 0x14100050 4 --width 4
DM32_PORT=/dev/ttyUSB0 python3 tools/debug/fkey.py sk1
```

按键检测流程见第 11 节。

### 12.5 调试脚本 PATH 适配

`tools/debug/dm32dbg.py` 中的 `NM` 改为从 PATH 查找工具链：

```python
NM = os.environ.get('CSKY_NM', 'csky-elf-noneabiv2-nm')
```

这样 `fkey.py` 等依赖 `dm32dbg.NM` 的脚本可以直接使用 PATH 中的 `csky-elf-noneabiv2-nm`。

使用 `tools/release/make_release.py` 时，让 `csky-elf-noneabiv2-*` 和 `make` 都在 PATH 中即可。
