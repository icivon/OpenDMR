# OpenDMR 与 MMDVMHost / DMRGateway 联合使用指南

本文档说明如何在 Ubuntu 26.04 上，把已经编译安装好的 MMDVMHost 和 DMRGateway 与运行 OpenDMR 固件的 DM-32 / DM-32UV 配合使用。

本文场景是：

- Ubuntu 26.04 作为 DMR 热点主机；
- 电台通过原装 CH340 或兼容 FTDI 的写频线连接到 Ubuntu；
- 电台只充当 MMDVM Modem，不使用常规的模拟调制解调器硬件；
- MMDVMHost 负责 DMR 网络协议；
- DMRGateway 可按现有配置连接多个 BrandMeister、TGIF、DMR+ 等网络。

> 本功能仅适用于运行 OpenDMR 固件的 DM-32 / DM-32UV。原厂固件不能充当 MMDVM Modem。

## 1. 功能边界

OpenDMR 会把写频串口作为 MMDVM 串口使用。

当前固件支持：

- DMR 单工/DMO；
- MMDVMHost 的版本、状态、配置和频率请求；
- 网络发送到电台的 DMR 语音头、语音帧和结束帧；
- 电台接收的 DMR 语音发送到 MMDVMHost；
- 电台端的色码、频率、功率由 MMDVMHost 设置；
- 调用方和目标 TG/私呼的界面显示；
- MMDVMHost 20 秒没有活动时自动退出热点界面。

当前固件不支持：

- DMR 双工中继模式；
- DSTAR、YSF、P25、NXDN；
- POCSAG 分页内容发送，只能协商模式和状态；
- 主机断开后仍继续热点工作。

因此，MMDVM.ini 中应只启用 DMR，其余数字协议应关闭：

```ini
[DMR]
Enable=1

[D-Star]
Enable=0

[System Fusion]
Enable=0

[P25]
Enable=0

[NXDN]
Enable=0

[POCSAG]
Enable=0
```

## 2. 准备工作

### 2.1 电台侧

1. 确认电台已经刷入 OpenDMR 固件。
2. 在电台上进入：

   ```text
   通用设置 → 热点 → MMDVM
   ```

3. 关闭其他串口占用程序，包括：

   - OpenDMR CPS；
   - Python 写频/备份工具；
   - 串口终端；
   - 旧的 MMDVMHost 实例。

4. 用写频线连接 Ubuntu。线只用于串口和供电，不会安装固件或写码表。

串口参数固定为：

```text
波特率：115200
数据位：8
停止位：1
校验：无
流控：无
```

### 2.2 Ubuntu 串口

插入写频线后查看设备：

```bash
ls -l /dev/ttyUSB*
lsusb
```

常见设备是：

```text
/dev/ttyUSB0
/dev/ttyACM0
```

把运行 MMDVMHost 的用户加入串口组：

```bash
sudo usermod -aG dialout "$USER"
```

重新登录或重启后生效。也可以先临时检查：

```bash
sudo chmod 666 /dev/ttyUSB0
```

不要把这种临时权限作为长期方案；长期运行应使用 `dialout` 权限和可选的 udev 规则。

### 2.3 确认进程和可执行文件

先确认已安装的程序位置：

```bash
command -v MMDVMHost
command -v DMRGateway
```

常见位置是：

```text
/usr/local/bin/MMDVMHost
/usr/local/bin/DMRGateway
```

确认配置文件和 systemd 服务：

```bash
systemctl cat mmdvmhost.service 2>/dev/null
systemctl cat dmrgateway.service 2>/dev/null
sudo systemctl status mmdvmhost.service
sudo systemctl status dmrgateway.service
```

实际服务名可能是 `MMDVMHost`、`mmdvmhost`、`dmrgateway` 或手工配置的名称，以下面的文件和命令为准：

```bash
sudo systemctl list-units --type=service | grep -Ei 'mmdvm|dmr'
```

## 3. MMDVMHost 配置原则

下面以常见的 MMDVMHost 配置结构说明。应以实际文件为准，不要复制整份示例后覆盖已有 DMRGateway 配置。

### 3.1 Modem 串口

在 MMDVM.ini 中：

```ini
[Modem]
Protocol=UART
UARTProtocol=1
Port=/dev/ttyUSB0
LocalPort=0
RXInvert=0
TXInvert=0
PTTInvert=0
TXDelay=100
RXOffset=0
TXOffset=0
RFLevel=100
Debug=0
```

需要重点检查：

- `Port` 指向真实串口；
- 没有运行其他串口程序占用该设备；
- `UARTProtocol=1` 对当前多版本 MMDVMHost 属于常见设置；如你的已编译版本要求对应字段，以该版本的示例配置为准；
- `TXDelay` 单位通常是 10 ms；
- `RFLevel=100` 时固件使用电台本身的通用设置功率档；部分版本中也可以由该值换算功率。

### 3.2 单工模式

必须设置为非双工：

```ini
[General]
Duplex=0
```

频率应使用电台实际可以发射的频率，单位是 Hz：

```ini
[Info]
RXFrequency=430000000
TXFrequency=430000000

[DMR]
Enable=1
ColorCode=1
```

如果频率和发射频率相同，就是单工：

```text
RXFrequency = TXFrequency = 430000000
```

下面内容会被固件拒绝或无法正常工作：

- RX/TX 分离较远的双工配置；
- `Duplex=1`；
- 不合法的频率；
- 色码超出 0–15；
- 发射频率落入固件禁止的特定卫星频段。

### 3.3 DMR 协议

```ini
[DMR]
Enable=1
ColorCode=1
SelfOnly=0
EmbeddedLCOnly=0
DumpTAData=1
DumpRCData=1
```

对初次联调，建议：

```ini
Debug=1
Trace=1
```

放置于 `[Modem]` 或具体配置语法允许的区域，但不要打开大量日志持续运行；稳定后再关闭。

### 3.4 不启用其他协议

```ini
[D-Star]
Enable=0

[System Fusion]
Enable=0

[P25]
Enable=0

[NXDN]
Enable=0

[POCSAG]
Enable=0
```

## 4. 与 DMRGateway 连接

如果 MMDVMHost 已经成功连接 DMRGateway，通常不需要把 MMDVMHost 的网络端直接改成 BrandMeister 或其他服务器。

常见拓扑是：

```text
DM-32 / OpenDMR
      │
      │ 115200 8N1，MMDVM 串口协议
      ▼
MMDVMHost
      │
      │ DMR UDP 本地端口
      ▼
DMRGateway
      │
      ├── BrandMeister
      ├── TGIF
      └── DMR+ / XLX 等已配置网络
```

典型配置方式是：

- MMDVMHost 中的 `[DMR Network]` 指向本机 DMRGateway；
- DMRGateway 中继续保留已有的服务器、端口、密码和本机 DMR ID；
- 不要同时让 MMDVMHost 直接和其他 DMR 网络建立多个连接。

常见 MMDVMHost 侧示例：

```ini
[DMR Network]
Enable=1
Address=127.0.0.1
Port=62031
Local=62032
Password=PASSWORD
Debug=0
```

常见端口并不固定，端口号取决于已有 DMRGateway 配置。必须以实际文件中的端口配对为准，例如：

```text
MMDVMHost → DMRGateway：62031
DMRGateway 回程：62032
```

不要只按示例修改端口，应检查 DMRGateway 中类似配置：

```ini
[DMR Network]
Port=62031
Local=62032
```

或本机监听端口和名称可能出现在其他段名中。先读取当前成功配置：

```bash
sudo grep -nE '127\.0\.0\.1|62031|62032|Port=|Local=' /etc/DMRGateway.ini
sudo grep -nE '127\.0\.0\.1|62031|62032|Address=|Port=|Local=' /etc/MMDVM.ini
```

如果配置文件位于其他位置，使用：

```bash
sudo find /etc /opt /usr/local/etc -type f \( -iname '*mmdvm*.ini' -o -iname '*dmrgateway*.ini' \) 2>/dev/null
```

## 5. 在 Ubuntu 上启动

### 5.1 停止可能占用串口的服务

```bash
sudo systemctl stop mmdvmhost.service 2>/dev/null || true
sudo systemctl stop dmrgateway.service 2>/dev/null || true
```

停止前先确认服务名：

```bash
sudo systemctl list-units --type=service | grep -Ei 'mmdvm|dmr'
```

不要同时启动多个 MMDVMHost 实例。

### 5.2 手动前台启动联调

先启动 DMRGateway：

```bash
sudo DMRGateway /etc/DMRGateway.ini
```

如可执行文件或配置位于其他位置：

```bash
sudo /usr/local/bin/DMRGateway /path/to/DMRGateway.ini
```

再另开一个终端启动 MMDVMHost：

```bash
sudo MMDVMHost /etc/MMDVM.ini
```

通常需要手动启动时使用：

```bash
sudo /usr/local/bin/MMDVMHost /path/to/MMDVM.ini
```

前台启动便于观察：

- 串口打开失败；
- 权限拒绝；
- MMDVM 握手失败；
- DMRGateway UDP 无响应；
- DMR 收发无数据。

### 5.3 使用 systemd 运行

当手动启动正常后，再启用服务：

```bash
sudo systemctl daemon-reload
sudo systemctl enable dmrgateway.service mmdvmhost.service
sudo systemctl restart dmrgateway.service
sudo systemctl restart mmdvmhost.service

sudo systemctl status dmrgateway.service
sudo systemctl status mmdvmhost.service
```

查看日志：

```bash
sudo journalctl -u dmrgateway.service -f
sudo journalctl -u mmdvmhost.service -f
```

也可以查看 MMDVMHost 日志文件：

```bash
sudo tail -f /var/log/MMDVM-*.log
sudo tail -f /var/log/mmdvmhost.log 2>/dev/null
```

日志文件名取决于 MMDVM.ini 中的 `[Log]` 设置。

## 6. 联调步骤

1. 电台正常开机。
2. 在电台菜单设置：

   ```text
   通用设置 → 热点 → MMDVM
   ```

3. 插好串口线。
4. 启动 DMRGateway。
5. 启动 MMDVMHost。
6. 等待串口握手。正常情况下，MMDVMHost 会收到 OpenDMR 返回的：

   ```text
   GET_STATUS
   GET_VERSION
   ACK
   ```

7. 电台打开热点界面后，界面可能先显示：

   ```text
   等待 Pi-Star
   ```

8. MMDVMHost 正常通信后，界面可能显示：

   - 热点模式；
   - 色码；
   - 接收或发射频率；
   - 发射功率；
   - 主机 IP；
   - 呼叫源、TG 或私呼目标。

主机只要继续发数据，热点就保持运行。MMDVMHost 没有串口数据超过约 20 秒，或服务停止通信，OpenDMR 会退出热点界面并恢复原频道。

## 7. 发起测试呼叫

联调时建议先在电台上从一个普通 FM 信道切换到 DMR 单工功能，并选择：

- 一个有效的 DMR 信道；
- 正确的色码；
- 正确的发射频率；
- 可用的发射功率。

实际发射频率不由电台码表决定，热点界面运行时由 MMDVMHost 下发。

较安全的测试顺序：

1. 用另一台调频接收机确认电台发射频率；
2. 先在低功率测试；
3. 在 MMDVMHost 日志中确认收到 DMR 数据；
4. 在 DMRGateway 日志中确认数据没有丢失；
5. 再测试网络回传语音；
6. 最后测试常用 TG、私呼和跨网路由。

## 8. 串口动态分配

Ubuntu 重新启动或重新插线后，串口可能由 `/dev/ttyUSB0` 变成 `/dev/ttyUSB1`。

建议使用稳定的串口别名，例如：

```bash
udevadm info -q property -n /dev/ttyUSB0 | grep -E 'ID_SERIAL|ID_VENDOR|ID_MODEL'
```

创建规则：

```bash
sudo tee /etc/udev/rules.d/99-opendmr-usbserial.rules >/dev/null <<'EOF'
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", SYMLINK+= "opendmr", MODE="0660", GROUP="dialout"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
```

上述 `1a86:7523` 是常见 CH340 USB 串口芯片的 VID/PID。实际设备可能不同，以 `udevadm` 显示的值为准。

再把 MMDVM.ini 改为：

```ini
Port=/dev/opendmr
```

重新插拔设备或重启：

```bash
sudo reboot
```

重建服务前确认设备：

```bash
ls -l /dev/opendmr
```

## 9. 固件已知限制

OpenDMR 对 MMDVM 模式进行了以下限制：

- 热点运行时会覆盖目的过滤、色码和时隙过滤；
- 热点退出后恢复运行前的过滤设置；
- `RFLevel=255` 或主机未提供有效值时使用电台设置中的功率档；
- 默认频率为 `430.00000 MHz`，直到主机下发有效频率；
- 当前不实现中继型双工；
- DMR 呼叫只接受单工状态机；
- P25/NXDN 不处理，会向主机返回 NAK；
- CW ID 以 FM 调制发送，而不是数字 DMR 数据；
- 写频功能在热点运行时仍可继续使用。

相关实现位于：

```text
src/services/hotspot.c
src/services/cable_link.c
src/app/hotspot_screen.c
src/dmr/hr_c7000.c
```

## 10. 日志和诊断

### 10.1 检查串口占用

```bash
sudo fuser -v /dev/ttyUSB0
sudo lsof /dev/ttyUSB0
```

如果被其他进程占用：

```bash
sudo fuser -k /dev/ttyUSB0
```

### 10.2 检查串口回环

不要在 MMDVMHost 或其他串口程序运行时直接做回环测试，因为会影响握手和帧流。

测试时先停止 MMDVMHost：

```bash
sudo systemctl stop mmdvmhost.service
```

查看串口数据：

```bash
sudo apt install -y minicom
sudo minicom -D /dev/ttyUSB0 -b 115200
```

正常联调时，不建议在终端中直接发送任何内容。串口回环或人工字符可能破坏 MMDVM 帧解析。

### 10.3 启用调试日志

MMDVM.ini 可按需要启用：

```ini
[Modem]
Debug=1

[Log]
FileLevel=2
ConsoleLevel=2

[DMR]
Enable=1
Debug=1
```

字段是否存在取决于实际 MMDVMHost 版本。启用后先运行短时间，再关闭，避免日志占满磁盘。

查看日志：

```bash
sudo journalctl -u mmdvmhost.service --since '-10 minutes'
sudo grep -RniE 'DMR|MODEM|serial|frame|ack|nak|timeout' /var/log/MMDVM* 2>/dev/null
```

### 10.4 常见问题

#### 电台没有进入热点界面

检查：

- 电台热点设置是否为 MMDVM；
- 写频线是否真正插到底；
- `/dev/ttyUSB0` 是否正确；
- MMDVMHost 是否启动且串口未被占用；
- Ubuntu 是否有 dialout 权限；
- MMDVM.ini 中 `Duplex` 是否等于 0；
- DMR 是否已启用；
- MMDVMHost 是否已经在向串口发送握手帧。

#### MMDVMHost 能启动但电台不回应

检查：

- 串口芯片是否是 CH340/FTDI 并支持稳定的 115200；
- 协议是否使用 UART，而不是 TCP 模拟串口；
- 电台是否仍被 CPS 占用；
- 波特率是否被动态提高到超过 115200；正常运行时由固件返回；
- 连接线是否被 USB 集线器供电不足或数据线不稳。

#### DMRGateway 已连接，但无语音

检查：

- MMDVMHost `[DMR Network]` 地址和端口；
- DMRGateway 本地端口是否与 MMDVMHost 相配；
- 防火墙是否拦截本机 UDP；
- DMR ID 和密码是否正确；
- TG、分组和目标网络是否正确；
- 电台配置的色码是否与 MMDVM.ini 中一致；
- DMRGateway 是否启用了目标网络。

查看本机 UDP 端口：

```bash
sudo ss -lunp | grep -E '62031|62032|4000|4001'
```

端口以实际配置为准。

#### 无法发射或网络呼叫不回传

检查：

- 电台本地射频是否被禁止；
- 电台是否设置为蓝牙、局部限频或仅接收；
- `RXFrequency/TXFrequency` 是否在允许范围；
- 天线是否正常；
- DMRGateway 是否收到发射帧；
- MMDVMHost 是否收到电台端 DMR 数据；
- 日志中是否有 ACK、NAK 或 timeout。

#### 热点运行一段时间后退出

正常情况下，MMDVMHost 或串口超过约 20 秒没有有效数据，OpenDMR 会退出热点状态。

检查：

- DMRGateway 是否中断；
- MMDVMHost 是否被系统休眠或停止；
- 串口是否断开；
- systemd 服务是否崩溃；
- 日志中是否有串口打开失败或网络超时。

#### 私呼或部分呼叫不显示

检查：

- MMDVMHost 是否提供 QSO 信息帧；
- DMR 名单是否可用；
- 私呼目标是否被正确解析；
- 电台是否收到完整的 LC；
- DMRGateway 是否传入完整呼叫。

## 11. 推荐的最小联调配置

仅用于验证串口和 DMR 单工链路时，建议先用最小配置：

```ini
[General]
Duplex=0

[Info]
RXFrequency=430000000
TXFrequency=430000000

[Modem]
Protocol=UART
UARTProtocol=1
Port=/dev/ttyUSB0
RXInvert=0
TXInvert=0
PTTInvert=0
TXDelay=100
RFLevel=100

[DMR]
Enable=1
ColorCode=1

[D-Star]
Enable=0

[System Fusion]
Enable=0

[P25]
Enable=0

[NXDN]
Enable=0

[POCSAG]
Enable=0

[DMR Network]
Enable=1
Address=127.0.0.1
Port=<与 DMRGateway 实际监听端口一致>
Local=<与 DMRGateway 回程端口一致>
Password=<已有正确密码>
```

联调成功后再恢复完整的：

- BrandMeister/TGIF 路由；
- 白名单或黑名单；
- Callsign 和位置显示；
- 日志级别；
- 多网络时隙映射；
- TG 静态与动态映射。

## 12. 快速启动清单

```bash
# 1. 识别串口
ls -l /dev/ttyUSB*

# 2. 确认服务
sudo systemctl list-units --type=service | grep -Ei 'mmdvm|dmr'

# 3. 停止旧实例并检查占用
sudo systemctl stop mmdvmhost.service 2>/dev/null || true
sudo fuser -v /dev/ttyUSB0

# 4. 启动网关和主机
sudo DMRGateway /etc/DMRGateway.ini
# 另开终端：
sudo MMDVMHost /etc/MMDVM.ini

# 5. 查看日志
sudo journalctl -u dmrgateway.service -f
sudo journalctl -u mmdvmhost.service -f

# 6. 系统正常后启用服务
sudo systemctl enable dmrgateway.service mmdvmhost.service
sudo systemctl restart dmrgateway.service
sudo systemctl restart mmdvmhost.service
```

最后确认：

- Ubuntu 已正确启动 DMRGateway 和 MMDVMHost；
- 串口没有被其他程序占用；
- 电台热点模式已设置为 MMDVM；
- MMDVM.ini 为 DMR 单工；
- DMRGateway 端口与 MMDVMHost 网络端完全一致；
- 电台界面进入热点模式并能显示频率、色码和功率。
