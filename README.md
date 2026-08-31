# ESP32-S3 多子项目工作区

## 结构

```
esp32-s3/
├── idf.ps1                # 统一 CLI 入口（唯一需要记住的命令）
├── projects/              # 所有 ESP-IDF 子项目，一目录一子项目
│   ├── wifi_sniffer/      # WiFi 嗅探固件（log/PCAP 双模式）+ tools/ 串口抓包分析脚本
│   ├── flash_dump/        # SPI Flash 整片 dump 固件 + tools/flash_dump.py
│   └── usb_uart_bridge/   # 双路 USB-CDC ↔ UART 透传桥（TinyUSB 复合设备）
└── .vscode/               # 仅 IDF 路径等基础配置，不做 GUI 项目选择
```

## 使用（全部命令行）

```powershell
.\idf.ps1 list                                    # 列出子项目
.\idf.ps1 wifi_sniffer build                      # 构建
.\idf.ps1 wifi_sniffer -p COM20 -b 460800 flash monitor
.\idf.ps1 flash_dump -p COM20 flash
.\idf.ps1 usb_uart_bridge -p COM20 flash
# 其余 idf.py 参数原样透传：menuconfig / clean / fullclean / size ...
```

- 脚本自动激活 `E:\Espressif\esp-idf` 环境（幂等），新终端直接可用。
- 各子项目配置只改自己的 `sdkconfig.defaults`；改后删除该子项目的 `sdkconfig` 再构建才生效。

## 新增子项目（扩展性约定）

1. `projects/<name>/` 下放 `CMakeLists.txt`（含 `project(<name>)`）+ `main/`。
2. PC 端配套脚本放 `projects/<name>/tools/`。
3. 无需改动 `idf.ps1`，`.\idf.ps1 <name> build` 即可使用。

## 子项目要点

- **wifi_sniffer**: stdin `p`/`q` 或按住 BOOT 上电切 PCAP/log 模式；控制台 921600；PCAP linktype=127，时间戳为开机 µs。
- **flash_dump**: 目标为板上 16MB 博雅 QSPI Flash（P25Q 系列，256/512KB），`tools/flash_dump.py` 按帧校验整片拉取。
- **usb_uart_bridge**: 原生 USB（GPIO19/20）枚举 2×CDC 复合设备，分别透传到 UART1(GPIO17/18)、UART2(GPIO15/16)；TinyUSB，免驱动。
