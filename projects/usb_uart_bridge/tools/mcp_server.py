#!/usr/bin/env python3
"""
usb_uart_bridge MCP 服务（stdio, 零依赖）

把 6 路复位输出 / 电平读取 / 空闲模式封装为 MCP 工具，供 AI Agent 调用。

依赖: pyserial  (pip install pyserial)
协议: docs/at-rst-control.md

用法（由 MCP 客户端拉起，无需手动运行）:
    python mcp_server.py [--port COM23] [--baud 115200]

手动自检:
    python mcp_server.py --selftest
"""
from __future__ import annotations

import json
import os
import sys
import threading
import time
from typing import Any, Dict, List, Optional

import serial
import serial.tools.list_ports

# ---------------------------------------------------------------- 设备常量

VID_PID = (0x303A, 0x4002)          # ESP32-S3 复合设备(2×CDC)
DEFAULT_BAUD = 115200
CMD_TIMEOUT = 0.8
MAX_INTERVAL = 60000
MAX_PULSE = 60000
RESET_PINS = {1: 4, 2: 5, 3: 6, 4: 7, 5: 8, 6: 9}

SERVER_NAME = "usb-uart-bridge"
SERVER_VERSION = "1.0.0"

# ---------------------------------------------------------------- 串口控制


class BridgeError(RuntimeError):
    pass


class RstController:
    """AT+RST 协议客户端。线程安全（内部加锁）。"""

    def __init__(self, port: Optional[str] = None, baud: int = DEFAULT_BAUD):
        self._lock = threading.Lock()
        self.port = port or self._autodetect()
        self.ser = serial.Serial(self.port, baud, timeout=0.2)
        self.ser.dtr = True
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self.interval = 100
        self.idle_mode = "HIGHZ"
        self.pulse = {p: 100 for p in RESET_PINS}
        self.default_level = {p: 0 for p in RESET_PINS}

    @staticmethod
    def _autodetect() -> str:
        """按 VID:PID 查找设备端口，未找到则回退到第一个可用串口。"""
        cands = []
        for p in serial.tools.list_ports.comports():
            if (p.vid, p.pid) == VID_PID:
                cands.append(p.device)
        if cands:
            return sorted(cands)[0]
        raise BridgeError(
            "未找到 usb_uart_bridge 设备（VID:PID=303A:4002）。"
            "请确认已插好，或用 --port 显式指定。"
        )

    # -- 底层收发 --------------------------------------------------

    def _send(self, cmd: str, timeout: float = CMD_TIMEOUT,
              expect=("OK", "+RSTCFG:", "+RSTMODE:", "+RSTLVL:", "ERROR")) -> Optional[str]:
        with self._lock:
            self.ser.reset_input_buffer()
            self.ser.write(cmd.encode() + b"\r")   # 单个 CR：CRLF 会向下游 UART 泄漏 \n
            self.ser.flush()
            deadline = time.time() + timeout
            buf = b""
            while time.time() < deadline:
                buf += self.ser.read(256)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.rstrip(b"\r").decode(errors="replace")
                    if text.startswith(expect):
                        return text
            return None

    def _ok(self, cmd: str) -> bool:
        return self._send(cmd) == "OK"

    # -- 工具实现 --------------------------------------------------

    def ping(self) -> Dict[str, Any]:
        return {"port": self.port, "ok": self._ok("AT")}

    def query(self) -> Dict[str, Any]:
        r = self._send("AT+RSTCFG?")
        if not r or not r.startswith("+RSTCFG:"):
            raise BridgeError("AT+RSTCFG? 无有效响应")
        raw: Dict[str, Any] = {}
        for kv in r[len("+RSTCFG:"):].split(","):
            k, _, v = kv.partition("=")
            raw[k] = v if k == "IDLE" else int(v)
        self.interval = raw["INTERVAL"]
        self.idle_mode = raw.get("IDLE", "HIGHZ")
        for p in RESET_PINS:
            self.pulse[p] = raw[f"PULSE{p}"]
            self.default_level[p] = raw[f"DEFAULT{p}"]

        pins = [
            {
                "pin": p,
                "gpio": RESET_PINS[p],
                "pulse_ms": self.pulse[p],
                "default_level": self.default_level[p],
            }
            for p in RESET_PINS
        ]
        return {
            "port": self.port,
            "interval_ms": self.interval,
            "idle_mode": self.idle_mode,
            "pins": pins,
            "raw": r,
        }

    def configure(self, interval_ms: int, pulse_ms: int, default_level: int) -> Dict[str, Any]:
        if not 0 <= interval_ms <= MAX_INTERVAL:
            raise ValueError(f"interval_ms 超出 0..{MAX_INTERVAL}")
        if not 1 <= pulse_ms <= MAX_PULSE:
            raise ValueError(f"pulse_ms 超出 1..{MAX_PULSE}")
        if default_level not in (0, 1):
            raise ValueError("default_level 只能为 0 或 1")
        ok = self._ok(f"AT+RSTCFG={interval_ms},{pulse_ms},{default_level}")
        if ok:
            self.interval = interval_ms
            for p in RESET_PINS:
                self.pulse[p] = pulse_ms
                self.default_level[p] = default_level
        return {"ok": ok, "scope": "all", "interval_ms": interval_ms,
                "pulse_ms": pulse_ms, "default_level": default_level}

    def configure_pin(self, pin: int, pulse_ms: int, default_level: int,
                      interval_ms: Optional[int] = None) -> Dict[str, Any]:
        if pin not in RESET_PINS:
            raise ValueError("pin 只能是 1..6")
        interval_ms = self.interval if interval_ms is None else interval_ms
        if not 0 <= interval_ms <= MAX_INTERVAL:
            raise ValueError(f"interval_ms 超出 0..{MAX_INTERVAL}")
        if not 1 <= pulse_ms <= MAX_PULSE:
            raise ValueError(f"pulse_ms 超出 1..{MAX_PULSE}")
        if default_level not in (0, 1):
            raise ValueError("default_level 只能为 0 或 1")
        ok = self._ok(f"AT+RSTCFG={interval_ms},{pin},{pulse_ms},{default_level}")
        if ok:
            self.interval = interval_ms
            self.pulse[pin] = pulse_ms
            self.default_level[pin] = default_level
        return {"ok": ok, "scope": "pin", "pin": pin, "gpio": RESET_PINS[pin],
                "interval_ms": interval_ms, "pulse_ms": pulse_ms,
                "default_level": default_level}

    def reset(self, order: str = "1",
              settle_ms: Optional[int] = None) -> Dict[str, Any]:
        """order: '1'..'6' 的任意不重复排列；也接受 ALL / BOTH。"""
        order = str(order).strip().upper()
        if order == "BOTH":
            order = "12"
        elif order == "ALL":
            order = "123456"
        if not order or len(order) > len(RESET_PINS):
            raise ValueError("order 长度必须为 1..6")
        if len(set(order)) != len(order):
            raise ValueError("order 中数字不能重复")
        if any(c not in "123456" for c in order):
            raise ValueError("order 只能包含 1..6")

        sequence = []
        for c in order:
            p = int(c)
            sequence.append({"pin": p, "gpio": RESET_PINS[p],
                             "pulse_ms": self.pulse.get(p, 100)})

        # 预估总耗时（本地估算，用于等待 OK）
        total = sum(s["pulse_ms"] for s in sequence) + (len(sequence) - 1) * self.interval
        ok = self._send(f"AT+RST={order}", total / 1000.0 + CMD_TIMEOUT) == "OK"
        # 等待目标 MCU 启动（默认不额外等待，由调用方决定）
        if ok and settle_ms:
            time.sleep(settle_ms / 1000.0)
        return {"ok": ok, "order": order, "sequence": sequence,
                "estimated_duration_ms": total}

    def read_levels(self) -> Dict[str, Any]:
        r = self._send("AT+RSTLVL")
        if not r or not r.startswith("+RSTLVL:"):
            raise BridgeError("AT+RSTLVL 无有效响应")
        levels: Dict[int, int] = {}
        for kv in r[len("+RSTLVL:"):].split(","):
            k, _, v = kv.partition("=")
            if k.startswith("P"):
                levels[int(k[1:])] = int(v)
        pins = [{"pin": p, "gpio": RESET_PINS[p], "level": levels.get(p)}
                for p in RESET_PINS]
        return {"port": self.port, "idle_mode": self.idle_mode,
                "levels": levels, "pins": pins, "raw": r}

    def read_level(self, pin: int) -> Dict[str, Any]:
        if pin not in RESET_PINS:
            raise ValueError("pin 只能是 1..6")
        r = self._send(f"AT+RSTLVL={pin}")
        if not r or not r.startswith("+RSTLVL:"):
            raise BridgeError(f"AT+RSTLVL={pin} 无有效响应")
        return {"pin": pin, "gpio": RESET_PINS[pin],
                "level": int(r.rsplit("=", 1)[1])}

    def get_mode(self) -> Dict[str, Any]:
        r = self._send("AT+RSTMODE?")
        if not r or not r.startswith("+RSTMODE:"):
            raise BridgeError("AT+RSTMODE? 无有效响应")
        self.idle_mode = r[len("+RSTMODE:"):]
        return {"idle_mode": self.idle_mode}

    def set_mode(self, high_z: bool) -> Dict[str, Any]:
        ok = self._ok(f"AT+RSTMODE={1 if high_z else 0}")
        if ok:
            self.idle_mode = "HIGHZ" if high_z else "DRIVE"
        return {"ok": ok, "idle_mode": self.idle_mode}

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


# ---------------------------------------------------------------- MCP 工具表

TOOLS: List[Dict[str, Any]] = [
    {
        "name": "bridge_ping",
        "description": "检查 usb_uart_bridge 设备连通性（发送 AT，期望 OK）。",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "bridge_query_config",
        "description": (
            "查询 6 路复位输出的完整配置：全局间隔、空闲模式(HIGHZ/DRIVE)、"
            "每个脚对应的 GPIO、脉冲宽度、默认电平。"
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "bridge_configure_all",
        "description": (
            "统一设置全部 6 路的参数（间隔、脉冲宽度、默认电平）。"
            "注意：会同时改变六个引脚的空闲电平（仅 DRIVE 模式下立即生效）。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "interval_ms": {"type": "integer", "minimum": 0, "maximum": MAX_INTERVAL,
                                "description": "多路复位时相邻脉冲之间的等待时间(ms)"},
                "pulse_ms": {"type": "integer", "minimum": 1, "maximum": MAX_PULSE,
                             "description": "复位脉冲宽度(ms)"},
                "default_level": {"type": "integer", "enum": [0, 1],
                                  "description": "引脚空闲/默认电平；复位脉冲为其反相"},
            },
            "required": ["interval_ms", "pulse_ms", "default_level"],
            "additionalProperties": False,
        },
    },
    {
        "name": "bridge_configure_pin",
        "description": "只设置单个复位脚的脉冲宽度与默认电平，不影响其他脚。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pin": {"type": "integer", "enum": [1, 2, 3, 4, 5, 6],
                        "description": "脚号 1..6 对应 GPIO4..GPIO9"},
                "pulse_ms": {"type": "integer", "minimum": 1, "maximum": MAX_PULSE},
                "default_level": {"type": "integer", "enum": [0, 1]},
                "interval_ms": {"type": "integer", "minimum": 0, "maximum": MAX_INTERVAL,
                                "description": "省略则沿用当前全局间隔"},
            },
            "required": ["pin", "pulse_ms", "default_level"],
            "additionalProperties": False,
        },
    },
    {
        "name": "bridge_reset",
        "description": (
            "执行复位脉冲。order 为 1..6 的不重复排列，按顺序依次复位；"
            "如 \"1\" 只复位1号脚，\"456\" 依次复位4→5→6，\"ALL\" 复位全部六路，"
            "\"BOTH\" 等价 \"12\"。响应在脉冲序列结束后才返回。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "order": {"type": "string", "default": "1",
                          "description": "复位顺序，如 '1' / '12' / '456' / 'ALL' / 'BOTH'"},
                "settle_ms": {"type": "integer", "minimum": 0,
                              "description": "复位序列结束后额外等待目标启动的毫秒数，默认 0"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "bridge_read_levels",
        "description": (
            "一次读取全部 6 个复位脚的当前电平。HIGHZ 空闲模式下读到的是目标板真实电平"
            "（本设备不驱动），可用于判断目标 MCU 复位脚状态；DRIVE 模式下读到的是本机驱动电平。"
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "bridge_read_level",
        "description": "读取单个复位脚的当前电平（0 或 1）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pin": {"type": "integer", "enum": [1, 2, 3, 4, 5, 6],
                        "description": "脚号 1..6 对应 GPIO4..GPIO9"},
            },
            "required": ["pin"],
            "additionalProperties": False,
        },
    },
    {
        "name": "bridge_get_idle_mode",
        "description": "查询引脚空闲模式：HIGHZ(平时高阻不驱动) 或 DRIVE(持续输出默认电平)。",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "bridge_set_idle_mode",
        "description": (
            "设置引脚空闲模式。high_z=true → HIGHZ，平时高阻且可读取目标真实电平（推荐）；"
            "high_z=false → DRIVE，平时持续输出默认电平。不持久化，重启恢复 HIGHZ。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"high_z": {"type": "boolean", "default": True}},
            "additionalProperties": False,
        },
    },
]

TOOL_NAMES = {t["name"] for t in TOOLS}

# ---------------------------------------------------------------- 会话状态


class Session:
    def __init__(self, port: Optional[str], baud: int):
        self.args = {"port": port, "baud": baud}
        self._dev: Optional[RstController] = None
        self._lock = threading.Lock()

    def device(self) -> RstController:
        with self._lock:
            if self._dev is None:
                self._dev = RstController(self.args["port"], self.args["baud"])
            return self._dev

    def reset_device(self):
        with self._lock:
            if self._dev:
                self._dev.close()
            self._dev = None

    def call(self, name: str, args: Dict[str, Any]) -> Any:
        if name == "bridge_ping":
            return self.device().ping()
        if name == "bridge_query_config":
            return self.device().query()
        if name == "bridge_configure_all":
            return self.device().configure(
                int(args["interval_ms"]), int(args["pulse_ms"]), int(args["default_level"]))
        if name == "bridge_configure_pin":
            return self.device().configure_pin(
                int(args["pin"]), int(args["pulse_ms"]), int(args["default_level"]),
                None if args.get("interval_ms") is None else int(args["interval_ms"]))
        if name == "bridge_reset":
            return self.device().reset(str(args.get("order", "1")),
                                       args.get("settle_ms"))
        if name == "bridge_read_levels":
            return self.device().read_levels()
        if name == "bridge_read_level":
            return self.device().read_level(int(args["pin"]))
        if name == "bridge_get_idle_mode":
            return self.device().get_idle_mode()
        if name == "bridge_set_idle_mode":
            return self.device().set_mode(bool(args.get("high_z", True)))
        raise ValueError(f"未知工具: {name}")


# ---------------------------------------------------------------- JSON-RPC


def _log(msg: str):
    print(f"[{SERVER_NAME}] {msg}", file=sys.stderr, flush=True)


def _ensure_utf8_stdio():
    """MCP 客户端按 UTF-8 解析；Windows 默认可能是 GBK，强制切到 UTF-8。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass


def _write(obj: Dict[str, Any]):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def _result(id_: Any, result: Any):
    _write({"jsonrpc": "2.0", "id": id_, "result": result})


def _error(id_: Any, code: int, message: str, data: Any = None):
    err = {"code": code, "message": message}
    if data is not None:
        err["data"] = data
    _write({"jsonrpc": "2.0", "id": id_, "error": err})


def handle(msg: Dict[str, Any], session: Session) -> None:
    method = msg.get("method")
    id_ = msg.get("id")
    params = msg.get("params") or {}

    if method == "initialize":
        _result(id_, {
            "protocolVersion": params.get("protocolVersion", "2024-11-05"),
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })
        return

    if method in ("notifications/initialized", "initialized"):
        return

    if method == "ping":
        _result(id_, {})
        return

    if method == "tools/list":
        _result(id_, {"tools": TOOLS})
        return

    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        if name not in TOOL_NAMES:
            _error(id_, -32602, f"未知工具: {name}")
            return
        try:
            out = session.call(name, args)
            _result(id_, {
                "content": [{"type": "text",
                             "text": json.dumps(out, ensure_ascii=False, indent=2)}],
                "isError": False,
            })
        except (ValueError, BridgeError) as e:
            _result(id_, {
                "content": [{"type": "text", "text": f"错误: {e}"}],
                "isError": True,
            })
        except serial.SerialException as e:
            session.reset_device()
            _result(id_, {
                "content": [{"type": "text",
                             "text": f"串口错误: {e}（已释放句柄，下次调用将重连）"}],
                "isError": True,
            })
        except Exception as e:  # noqa: BLE001
            _log(f"工具 {name} 异常: {e!r}")
            _result(id_, {
                "content": [{"type": "text", "text": f"内部错误: {e}"}],
                "isError": True,
            })
        return

    if id_ is not None:
        _error(id_, -32601, f"未实现的方法: {method}")


def serve(port: Optional[str], baud: int) -> int:
    session = Session(port, baud)
    _log(f"started (port={port or 'auto'}, baud={baud})")
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            _error(None, -32700, "JSON 解析失败")
            continue
        if isinstance(msg, list):
            for m in msg:
                handle(m, session)
        else:
            handle(msg, session)
    session.reset_device()
    return 0


# ---------------------------------------------------------------- 自检

def selftest(port: Optional[str], baud: int) -> int:
    dev = RstController(port, baud)
    print(f"port: {dev.port}")
    print("ping  :", dev.ping())
    print("config:", json.dumps(dev.query(), ensure_ascii=False))
    print("mode  :", dev.get_mode())
    print("levels:", json.dumps(dev.read_levels(), ensure_ascii=False))
    dev.close()
    return 0


def main() -> int:
    _ensure_utf8_stdio()
    port = None
    baud = DEFAULT_BAUD
    do_selftest = False

    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--port" and i + 1 < len(argv):
            port = argv[i + 1]
            i += 2
        elif a == "--baud" and i + 1 < len(argv):
            baud = int(argv[i + 1])
            i += 2
        elif a == "--selftest":
            do_selftest = True
            i += 1
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            _log(f"忽略未知参数: {a}")
            i += 1

    if do_selftest:
        return selftest(port, baud)
    return serve(port, baud)


if __name__ == "__main__":
    sys.exit(main())
