# usb-uart-bridge-mcp

ESP32-S3 **usb_uart_bridge** 的 MCP 服务：把 6 路复位输出、6 路电平读取、空闲模式控制封装为 MCP 工具，供 AI Agent / Claude / Cursor / VS Code 等客户端调用。

只需 **pyserial**，不依赖 `mcp` SDK，纯标准库实现 JSON-RPC over stdio。

---

## 1. 拷到新电脑（三种方式，任选其一）

### 方式 A：直接用源码运行（最简单，无需安装）

```powershell
# 1) 安装唯一依赖
pip install pyserial

# 2) 自检（确认能连上设备）
python usb_uart_bridge_mcp\server.py --port COM23 --selftest

# 3) 注册到 MCP 客户端（见下文配置片段）
```

### 方式 B：pip 安装为命令（推荐，路径稳定）

```powershell
cd <解压目录>
pip install .
# 之后可直接用：
usb-uart-bridge-mcp --port COM23 --selftest
```

### 方式 C：离线 wheel（目标机器无网络）

在**有网**的机器上执行一次：

```powershell
pip download pyserial -d wheels
pip wheel . -d dist --no-deps
# 把 dist\*.whl 和 wheels\*.whl 一起拷走
```

目标机器：

```powershell
pip install --no-index --find-links wheels dist\usb_uart_bridge_mcp-1.1.0-py3-none-any.whl
```

---

## 2. MCP 客户端配置

### 通用（Cursor / Claude Desktop / 多数客户端）

```json
{
  "mcpServers": {
    "usb-uart-bridge": {
      "command": "python",
      "args": ["C:\\path\\to\\usb_uart_bridge_mcp\\server.py", "--port", "COM23"]
    }
  }
}
```

### pip 安装后（方式 B）

```json
{
  "mcpServers": {
    "usb-uart-bridge": {
      "command": "usb-uart-bridge-mcp",
      "args": ["--port", "COM23"]
    }
  }
}
```

### Linux / macOS

```json
{
  "mcpServers": {
    "usb-uart-bridge": {
      "command": "python3",
      "args": ["/path/to/server.py", "--port", "/dev/ttyACM0"]
    }
  }
}
```

### 不写死端口（自动发现）

省略 `--port` 时按 **VID:PID = 303A:4002** 自动查找设备。也可用环境变量：

```json
{
  "mcpServers": {
    "usb-uart-bridge": {
      "command": "python",
      "args": ["C:\\path\\to\\server.py"],
      "env": { "USB_UART_BRIDGE_PORT": "COM23", "USB_UART_BRIDGE_BAUD": "115200" }
    }
  }
}
```

---

## 3. 提供的工具

| 工具 | 说明 |
|------|------|
| `bridge_ping` | 连通性检查（发 `AT`，期望 `OK`） |
| `bridge_query_config` | 查询全局间隔、空闲模式、六脚的 GPIO/脉宽/默认电平 |
| `bridge_configure_all` | 六路统一配置 `interval_ms, pulse_ms, default_level` |
| `bridge_configure_pin` | 单脚配置（可选 `interval_ms`，省略沿用当前值） |
| `bridge_reset` | 执行复位序列，`order` 为 1~6 的不重复排列，支持 `ALL`/`BOTH`；可选 `settle_ms` |
| `bridge_read_levels` | 一次读六脚电平 |
| `bridge_read_level` | 读单脚电平 |
| `bridge_get_idle_mode` | 查询空闲模式 `HIGHZ`/`DRIVE` |
| `bridge_set_idle_mode` | 设置空闲模式（`high_z: true/false`） |

### 返回示例

```json
{
  "port": "COM23",
  "idle_mode": "HIGHZ",
  "levels": { "1": 1, "2": 1, "3": 1, "4": 0, "5": 0, "6": 0 },
  "pins": [ { "pin": 1, "gpio": 4, "level": 1 }, ... ]
}
```

---

## 4. 硬件与协议

| 脚号 | 1 | 2 | 3 | 4 | 5 | 6 |
|------|---|---|---|---|---|---|
| GPIO | 4 | 5 | 6 | 7 | 8 | 9 |

- 设备枚举为 **2×CDC 复合设备**（VID:PID `303A:4002`），两个 COM 口行为一致，任选其一。
- 复位脚默认 **高阻空闲**：平时不驱动目标，仅在脉冲期间输出；此时读到的电平即目标板真实电平。
- AT 协议完整说明见固件仓库 `docs/at-rst-control.md`。
- 设备可能枚举出两个端口（如 COM23/COM24），指定任意一个即可。

---

## 5. 排错

| 现象 | 处理 |
|------|------|
| `未找到 usb_uart_bridge 设备` | 确认已插好；或用 `--port COMxx` 显式指定 |
| `串口错误 ... 已释放句柄` | 端口被其他程序占用（串口助手等），关闭后重试 |
| 工具返回 `isError: true` 但设备正常 | 检查是否另一进程占了同一端口 |
| 中文乱码 | 服务已强制 UTF-8 输出，若仍异常请设置 `PYTHONIOENCODING=utf-8` |
| 找不到 `pyserial` | `pip install pyserial` |

自检命令（不走 MCP，直接验证硬件通路）：

```powershell
python usb_uart_bridge_mcp\server.py --port COM23 --selftest
```

输出应包含 `ping : {'ok': True}` 与六路电平信息。

---

## 6. 文件说明

```
mcp-package/
├── pyproject.toml              # pip 安装配置，提供 usb-uart-bridge-mcp 命令
├── README.md                   # 本文件
├── setup.ps1 / setup.sh        # 一键安装并生成 mcp-fragment.json
├── make-package.ps1            # 重新打包成 usb-uart-bridge-mcp-<ver>.zip
└── usb_uart_bridge_mcp/
    ├── __init__.py
    ├── __main__.py             # python -m usb_uart_bridge_mcp
    └── server.py               # MCP 服务实现（也单文件可运行）
```

> `server.py` 是完整自包含脚本，复制这一个文件到任何有 Python + pyserial 的机器即可运行。

## 7. 重新打包分发 zip

仓库内改了 `mcp-package/` 后，在 `mcp-package/` 目录执行：

```powershell
.\make-package.ps1 -Version 1.1.0
# 产物: ..\usb-uart-bridge-mcp-1.1.0.zip
```

> `projects/usb_uart_bridge/tools/mcp_server.py` 只是**薄封装**，实际实现在本包的
> `usb_uart_bridge_mcp/server.py`，避免两份实现漂移。
