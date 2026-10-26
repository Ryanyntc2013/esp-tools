# usb_uart_bridge 功能清单

ESP32-S3 原生 USB 复合设备：**2 路虚拟串口 + 6 路复位控制 + 6 路电平读取**，单根 USB 线完成多台目标 MCU 的串口调试与复位管理。

> 面向自动化 Agent：协议说明书见 [at-rst-control.md](at-rst-control.md)，MCP 工具见 `../tools/mcp_server.py`。

---

## 1. 硬件资源总表

| 功能 | 引脚 | 说明 |
|------|------|------|
| USB D- / D+ | GPIO19 / GPIO20 | 原生 USB，复合设备 |
| UART1（CDC0） | TX=GPIO17, RX=GPIO18 | 3.3V TTL |
| UART2（CDC1） | TX=GPIO15, RX=GPIO16 | 3.3V TTL |
| 复位输出 1~6 | GPIO4 / 5 / 6 / 7 / 8 / 9 | 高阻空闲，仅脉冲期间驱动 |
| 控制台 | USB-Serial-JTAG | 与 TinyUSB 共存 |

**不占用**：SPI Flash（26–32）、PSRAM（33–37）、JTAG（39–42）、strapping（0/3/45/46）。

---

## 2. 双路虚拟串口桥

- 枚举为 **CDC 复合设备**，出现 2 个独立 COM 口，免驱（Win10+/Linux/macOS）。
- CDC0 ↔ UART1、CDC1 ↔ UART2，各自双向透传。
- 主机端波特率等参数自动同步到对应 UART（line coding 回调）。
- 每通道独立 8 KB 环形缓冲 + 独立收发任务，吸收 USB 空闲期突发。
- USB 口未打开（DTR=0）时该通道 UART→USB 数据丢弃，避免阻塞。

> USB 端点限制：`TINYUSB_CDC_COUNT` 最大为 2（EP0 + 2×CDC×3EP = 7）。

---

## 3. 六路复位输出

| 脚号 | 1 | 2 | 3 | 4 | 5 | 6 |
|------|---|---|---|---|---|---|
| GPIO | 4 | 5 | 6 | 7 | 8 | 9 |

**运行模型：高阻空闲（默认）**

```
空闲：输入模式 + 上下拉关闭（对外高阻，不干扰目标）
        ↓ 收到复位命令
脉冲：切推挽输出 → 驱动反相电平 → 保持 pulse_ms → 回高阻
```

- 上电即为高阻，**启动阶段不驱动任何复位脚**。
- 每脚**独立**的脉冲宽度与默认电平；多路序列的**间隔共享**。
- `AT+RSTMODE=0` 可切回"推挽空闲"（持续输出默认电平），不持久化。

**顺序复位**：`order` 为 1~6 的不重复排列，按序依次输出脉冲。

---

## 4. 六路电平读取

高阻模式下**输入缓冲器保持使能**，因此可以随时读取目标板真实电平而不影响它：

- `AT+RSTLVL` → 一次读全部六脚
- `AT+RSTLVL=<脚号>` → 读单脚

**典型用途**：判断目标 MCU 是否上电、复位脚是否被外部电路拉低、目标设备是否在位。

> `DRIVE` 模式下读到的是本机自己驱动的电平，参考价值有限。

---

## 5. AT 指令速查

| 命令 | 作用 |
|------|------|
| `AT` | 连通性测试 |
| `AT+RST=1`…`=6` | 单脚复位 |
| `AT+RST=<排列>` | 多路顺序复位，如 `12` / `456` / `654321` |
| `AT+RST=ALL` / `BOTH` | 六路全部 / 前两路（`12`） |
| `AT+RSTCFG?` | 查询全部配置 |
| `AT+RSTCFG=<间隔>,<脉冲>,<电平>` | 六路统一配置 |
| `AT+RSTCFG=<间隔>,<脚号>,<脉冲>,<电平>` | 单脚配置 |
| `AT+RSTMODE?` | 查询空闲模式 |
| `AT+RSTMODE=1` / `=0` | 高阻空闲（默认）/ 推挽空闲 |
| `AT+RSTLVL` / `AT+RSTLVL=<脚号>` | 读取电平 |
| `AT+DL` | 进入 USB 下载模式（无响应） |

**帧格式**：命令以**单个 `\r`** 结束（不要用 `\r\n`，会把 `\n` 泄漏到下游 UART）；响应统一以 `\r\n` 结尾。

---

## 6. 工具链

| 文件 | 用途 |
|------|------|
| `tools/reset_test.py` | 命令行验证：`ping` / `query` / `cfg` / `cfgpin` / `mode` / `lvl` / `reset` |
| `tools/mcp_server.py` | **MCP stdio 服务**，把全部功能暴露为 9 个 Agent 工具 |
| `tools/bridge_test.py` | 串口收发回环测试 |
| `tools/crossloop_test.py` | 双通道交叉互连测试 |
| `tools/bootlog_capture.py` | 目标启动日志抓取 |

### MCP 工具列表

| 工具 | 说明 |
|------|------|
| `bridge_ping` | 连通性检查 |
| `bridge_query_config` | 查询六路完整配置 |
| `bridge_configure_all` | 六路统一配置 |
| `bridge_configure_pin` | 单脚配置 |
| `bridge_reset` | 执行复位序列（可带 `settle_ms` 等待目标启动） |
| `bridge_read_levels` | 读取六脚电平 |
| `bridge_read_level` | 读取单脚电平 |
| `bridge_get_idle_mode` | 查询空闲模式 |
| `bridge_set_idle_mode` | 设置空闲模式 |

启动（由 MCP 客户端拉起，也可手动自检）：

```powershell
python tools/mcp_server.py --port COM23            # stdio 服务
python tools/mcp_server.py --port COM23 --selftest # 自检
```

未指定 `--port` 时按 `VID:PID = 303A:4002` 自动发现端口。

---

## 7. 构建 / 烧录

```powershell
.\idf.ps1 usb_uart_bridge build
.\idf.ps1 usb_uart_bridge -p COM20 flash
```

> 若设备正处于 USB CDC 运行模式，可先发 `AT+DL` 进入 ROM 下载模式（枚举为 COM8），烧完需重新插拔 USB 恢复。

---

## 8. 接线

| ESP32-S3 | 对端 | 说明 |
|----------|------|------|
| GPIO17 | 对端1 RX | UART1 TX |
| GPIO18 | 对端1 TX | UART1 RX |
| GPIO15 | 对端2 RX | UART2 TX |
| GPIO16 | 对端2 TX | UART2 RX |
| GPIO4~9 | 目标 MCU 复位脚 | 目标端建议 10 kΩ 上/下拉到其默认电平 |
| GND | GND | **必须共地** |
