# USB 复合设备：2 路虚拟串口 ↔ 2 路 3.3V TTL（类双口 CH340）

ESP32-S3 原生 USB（GPIO19 D-/GPIO20 D+）枚举为 **CDC 复合设备**，出现 2 个独立虚拟 COM 口，各自双向透传到一路 3.3V TTL UART；两个 COM 口都能解析 AT 指令并控制三个复位输出脚（3 台设备）。Win10+/Linux/macOS 免驱动即插即用；每个口的主机端波特率等参数独立同步到对应 UART。

> ESP32-S3 USB 端点数限制（EP0 + 2×CDC×3EP = 7），`TINYUSB_CDC_COUNT` 最大为 **2**，即最多 2 路虚拟串口。

## 通道映射

| 虚拟 COM | UART | TX 引脚 | RX 引脚 |
|----------|------|---------|---------|
| CDC0 (COMx) | UART1 | GPIO17 | GPIO18 |
| CDC1 (COMy) | UART2 | GPIO15 | GPIO16 |

## 双 CDC AT 控制

> **自动化 Agent 请直接阅读完整协议说明书：[docs/at-rst-control.md](docs/at-rst-control.md)**（含字节级帧格式、时序图、边界陷阱和参考实现）。

COM23（CDC0）和 COM24（CDC1）均支持以下 AT 指令。指令从哪个 COM 口发送，响应就从哪个 COM 口返回；普通非 AT 数据仍然透传到对应 UART。

复位输出默认使用 GPIO4 / GPIO5 / GPIO6（对应脚号 1 / 2 / 3），默认电平为低。复位时输出相反电平，保持该脚自身的 `PULSE` 毫秒后恢复默认电平；因此默认电平为低时，`PULSE` 就是高电平脉冲时间。脉冲宽度与默认电平**每脚独立**，多路序列的间隔**三路共享**。

### AT 指令

每条指令以单个 `CR` 或 `LF` 结束（不要用 `CRLF`，会把 `\n` 泄漏到 UART），返回 `OK` 或 `ERROR`：

| 指令 | 作用 |
|------|------|
| `AT` | 连通性测试 |
| `AT+RST=1` / `=2` / `=3` | 只复位对应脚（GPIO4 / GPIO5 / GPIO6） |
| `AT+RST=12` / `AT+RST=BOTH` | 先复位 GPIO4，再按间隔复位 GPIO5 |
| `AT+RST=21` | 先复位 GPIO5，再按间隔复位 GPIO4 |
| `AT+RST=123` / `AT+RST=ALL` | 依次复位 GPIO4 → GPIO5 → GPIO6 |
| `AT+RST=<排列>` | 1/2/3 的任意不重复排列，如 `312`、`31` |
| `AT+RSTCFG?` | 查询引脚、间隔、各脚脉冲时间和默认电平 |
| `AT+RSTCFG=<interval_ms>,<pulse_ms>,<default>` | 统一设置三路参数；`default` 只能为 `0` 或 `1` |
| `AT+RSTCFG=<interval_ms>,<pin>,<pulse_ms>,<default>` | 只设置单路参数；`pin` 为 1/2/3 |

示例：

```text
AT+RSTCFG=100,50,0      ; 三路统一：间隔100ms、脉冲50ms、空闲低
AT+RSTCFG=100,3,500,0   ; 仅 GPIO6：间隔100ms、脉冲500ms、空闲低
AT+RST=123              ; 三路依次复位
```

## 构建 / 烧录

```powershell
.\idf.ps1 usb_uart_bridge build
.\idf.ps1 usb_uart_bridge -p COM20 flash
```

## 接线（对端必须 3.3V 电平！）

| ESP32-S3 | 对端 |
|----------|------|
| GPIO17 (UART1 TX) | 对端1 RX |
| GPIO18 (UART1 RX) | 对端1 TX |
| GPIO15 (UART2 TX) | 对端2 RX |
| GPIO16 (UART2 RX) | 对端2 TX |
| GND | 共地（必须） |

## 注意

- AT 控制不再使用 UART0/COM20，两个虚拟 COM 均可直接控制复位。
- 每通道独立 8KB 环形缓冲 + 独立收发任务；USB 口未打开(DTR=0)时该通道 UART→USB 数据丢弃。
- 若需第 3 路：可再加 MSC/HID 等其他类组合，但 CDC 受端点数限制最多 2。
- GPIO4/GPIO5/GPIO6 必须连接到目标设备的复位输入，并确认目标设备与 ESP32-S3 共地；默认配置适用于复位脚空闲低、有效高的场景。
