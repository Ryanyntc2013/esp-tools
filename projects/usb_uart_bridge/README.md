# USB 复合设备：2 路虚拟串口 ↔ 2 路 3.3V TTL（类双口 CH340）

ESP32-S3 原生 USB（GPIO19 D-/GPIO20 D+）枚举为 **CDC 复合设备**，出现 2 个独立虚拟 COM 口，各自双向透传到一路 3.3V TTL UART。Win10+/Linux/macOS 免驱动即插即用；每个口的主机端波特率等参数独立同步到对应 UART。

> ESP32-S3 USB 端点数限制（EP0 + 2×CDC×3EP = 7），`TINYUSB_CDC_COUNT` 最大为 **2**，即最多 2 路虚拟串口。

## 通道映射

| 虚拟 COM | UART | TX 引脚 | RX 引脚 |
|----------|------|---------|---------|
| CDC0 (COMx) | UART1 | GPIO17 | GPIO18 |
| CDC1 (COMy) | UART2 | GPIO15 | GPIO16 |

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

- 控制台走 UART0（COM20），与两路桥接通道互不干扰。
- 每通道独立 8KB 环形缓冲 + 独立收发任务；USB 口未打开(DTR=0)时该通道 UART→USB 数据丢弃。
- 若需第 3 路：可再加 MSC/HID 等其他类组合，但 CDC 受端点数限制最多 2。
