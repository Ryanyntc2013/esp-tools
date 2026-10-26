#!/usr/bin/env python3
"""
usb_uart_bridge MCP 服务入口（仓库内薄封装）

真正的实现在 ../mcp-package/usb_uart_bridge_mcp/server.py —— 那份代码是
可分发的独立包（可 pip install / 整目录拷到其他电脑）。这里只是转调，
避免仓库内出现两份实现导致漂移。

用法:
    python mcp_server.py [--port COM23] [--baud 115200]
    python mcp_server.py --selftest

协议: docs/at-rst-control.md
依赖: pyserial
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_PKG_PARENT = os.path.normpath(os.path.join(_HERE, "..", "mcp-package"))

if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

from usb_uart_bridge_mcp.server import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
