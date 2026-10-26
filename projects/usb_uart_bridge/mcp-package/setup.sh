#!/usr/bin/env bash
# 一键安装 usb-uart-bridge MCP（Linux / macOS）
# 用法: ./setup.sh [--port /dev/ttyACM0] [--install]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG="$HERE/usb_uart_bridge_mcp/server.py"
PORT=""
INSTALL=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --port=*) PORT="${1#*=}"; shift ;;
    --install) INSTALL=1; shift ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

echo "== usb-uart-bridge-mcp 安装检查 =="

PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done
[[ -n "$PY" ]] || { echo "未找到 python3/python" >&2; exit 1; }
echo "[1/3] $($PY --version)"

if "$PY" -c "import serial" 2>/dev/null; then
  echo "[2/3] pyserial 已安装"
else
  echo "[2/3] 安装 pyserial ..."
  "$PY" -m pip install pyserial
fi

echo "[3/3] 设备自检 ..."
ARGS=("$PKG" "--selftest")
[[ -n "$PORT" ]] && ARGS+=("--port" "$PORT")
"$PY" "${ARGS[@]}" || echo "警告: 自检未通过（设备可能未连接）" >&2

# 生成配置片段
FRAG="$HERE/mcp-fragment.json"
"$PY" - "$FRAG" "$PKG" "$PORT" <<'PYEOF'
import json, sys
out, pkg, port = sys.argv[1], sys.argv[2], sys.argv[3]
args = [pkg] + (["--port", port] if port else [])
cfg = {"mcpServers": {"usb-uart-bridge": {"command": "python3", "args": args}}}
with open(out, "w", encoding="utf-8") as f:
    json.dump(cfg, f, ensure_ascii=False, indent=2)
PYEOF
echo "已生成配置片段: $FRAG"
echo "把其中内容合并到 MCP 客户端配置（如 ~/.cursor/mcp.json / claude_desktop_config.json）。"

if [[ "$INSTALL" == "1" ]]; then
  ( cd "$HERE" && "$PY" -m pip install . )
  echo "之后可用命令: usb-uart-bridge-mcp --port /dev/ttyACM0"
fi

echo "完成。"
