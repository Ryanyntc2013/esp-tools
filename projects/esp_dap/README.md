# ESP-DAP — 应急通用 DAP (HID + 1×CDC + nRST)

原生 USB (GPIO19/20) 复合设备：`HID 64B CMSIS-DAP v1` + `CDC 串口透传`，带目标复位。OpenOCD/pyOCD/Keil 即插即用。

> 定位应急探针，SWD 时钟 ~1MHz，下载 200~400KB/s，不替代 JLink

## 硬件接线 (3.3V ONLY)

| ESP32-S3 | 目标板 | 说明 |
|----------|--------|------|
| GPIO4 | SWDIO | 双向，需共地 |
| GPIO5 | SWCLK | |
| GPIO6 | nRESET | 低有效，开漏，悬空可省；若目标无复位脚可不接 |
| GPIO17 | 目标 RX | UART1 TX -> 目标 RX |
| GPIO18 | 目标 TX | UART1 RX -> 目标 TX |
| GND | GND | 必须共地 |
| 3V3 | — | **不供电**，目标需自供电，仅3.3V兼容 |

> 改引脚改 `swd.h` 的 `PIN_SWDIO/SWCLK/nRESET` 和 `main.c` 的 `UART_TX/RX` 即可

## 固件特性

- **HID 64B DAP v1**：`VID 303A:4005`，报告 64B，无需 WinUSB 驱动；`openocd -f interface/cmsis-dap.cfg` 直接识别
- **1×CDC**：`COMx / /dev/ttyACM0`，波特率自跟随，用于目标日志/烧录串口
- **SWD bit-bang**：支持 `DAP_Connect/SWD_Configure/Pins/Sequence/Transfer/ResetTarget/WriteABORT/Delay`，idle/重试可配
- **控制台** 走 `USB-Serial-JTAG (COM21)`，与 TinyUSB 互不干扰

## 构建 / 烧录

```powershell
.\idf.ps1 esp_dap build
.\idf.ps1 esp_dap -p COM20 flash monitor   # COM20 为 FTDI 烧录口，烧完复位即枚举 303A:4005
# 验证枚举：设备管理器应出现 HID 设备 + 一个 COM 口
```

## OpenOCD 使用

```powershell
# STM32F4 示例 (替换 target)
openocd -s tcl -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg -c "adapter speed 1000" -c "transport select swd"
# pyOCD
pyocd list
pyocd flash -t stm32f411ce firmware.elf
```
> ESP USB Bridge 仅支持 bulk，此探针为 HID，`cmsis-dap.cfg` 默认优先 HID，`v2` 亦兼容 bulk

- `adapter speed 1000` 起步，稳定后可试 `2000`，本探针 bit-bang 实测上限 ~2MHz
- 接 `nRST` 后可用 `reset_config srst_only` + `dap nreset` 控制
- CDC 口与烧录口不同：CDC 为原生 USB 枚举的虚拟串口，烧录时 `openocd` 占 HID，串口互不影响

## 调试建议

- 调试时关闭 WiFi/蓝牙任务，避免中断抖动
- 下载慢属正常；若 `WAIT` 较多降速至 500kHz
- 1.8V 目标需外加电平转换 (TXB0108)
