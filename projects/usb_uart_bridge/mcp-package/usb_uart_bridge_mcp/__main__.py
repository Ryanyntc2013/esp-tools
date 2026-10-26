"""``python -m usb_uart_bridge_mcp`` 入口。"""

import sys

from .server import main

if __name__ == "__main__":
    sys.exit(main())
