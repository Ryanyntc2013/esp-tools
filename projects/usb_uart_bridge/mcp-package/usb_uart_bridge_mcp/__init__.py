"""usb_uart_bridge MCP 服务包。

入口：``usb_uart_bridge_mcp.server:main``（也可 ``python -m usb_uart_bridge_mcp``）。
"""

from .server import (  # noqa: F401
    SERVER_NAME,
    SERVER_VERSION,
    RESET_PINS,
    BridgeError,
    RstController,
    TOOLS,
    main,
    serve,
    selftest,
)

__all__ = [
    "SERVER_NAME",
    "SERVER_VERSION",
    "RESET_PINS",
    "BridgeError",
    "RstController",
    "TOOLS",
    "main",
    "serve",
    "selftest",
]

__version__ = SERVER_VERSION
