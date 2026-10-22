#!/usr/bin/env python3
"""
usb_uart_bridge 复位控制测试工具（3 路：GPIO4/5/6）

用法:
  python reset_test.py COM23 ping
  python reset_test.py COM23 query
  python reset_test.py COM23 cfg 100,50,0            # 三路统一配置
  python reset_test.py COM23 cfgpin 3,500,0          # 只配置 3 号脚
  python reset_test.py COM23 reset 3                 # 只复位 GPIO6
  python reset_test.py COM23 reset 123               # 依次 GPIO4->5->6
  python reset_test.py COM23 reset ALL

依赖: pip install pyserial
协议说明: docs/at-rst-control.md
"""
import sys
import time

import serial

CMD_TIMEOUT = 0.5
MAX_INTERVAL = 60000
MAX_PULSE = 60000
RESET_PINS = {1: 4, 2: 5, 3: 6}


class RstController:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.ser.dtr = True
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self.interval = 100
        self.pulse = {p: 100 for p in RESET_PINS}
        self.default_level = {p: 0 for p in RESET_PINS}

    def _send(self, cmd, timeout, expect=("OK", "+RSTCFG:", "ERROR")):
        """发送命令并按预期前缀匹配响应，跳过桥接透传的无关数据"""
        self.ser.reset_input_buffer()
        # 单个 CR：用 CRLF 会把 \n 泄漏到下游 UART
        self.ser.write(cmd.encode() + b"\r")
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

    def ping(self):
        return self._send("AT", CMD_TIMEOUT) == "OK"

    def query(self):
        r = self._send("AT+RSTCFG?", CMD_TIMEOUT)
        if not r or not r.startswith("+RSTCFG:"):
            return None
        out = {}
        for kv in r[len("+RSTCFG:"):].split(","):
            k, _, v = kv.partition("=")
            out[k] = int(v)
        self.interval = out["INTERVAL"]
        for p in RESET_PINS:
            self.pulse[p] = out[f"PULSE{p}"]
            self.default_level[p] = out[f"DEFAULT{p}"]
        return out

    def configure(self, interval_ms, pulse_ms, default_level):
        """统一配置三路参数"""
        assert 0 <= interval_ms <= MAX_INTERVAL, "间隔越界"
        assert 1 <= pulse_ms <= MAX_PULSE, "脉冲越界"
        assert default_level in (0, 1), "默认电平只能为 0 或 1"
        r = self._send(f"AT+RSTCFG={interval_ms},{pulse_ms},{default_level}", CMD_TIMEOUT)
        if r == "OK":
            self.interval = interval_ms
            for p in RESET_PINS:
                self.pulse[p], self.default_level[p] = pulse_ms, default_level
            return True
        return False

    def configure_pin(self, pin, pulse_ms, default_level, interval_ms=None):
        """只配置单路；interval_ms 省略则沿用当前间隔"""
        assert pin in RESET_PINS, "脚号只能是 1/2/3"
        interval_ms = self.interval if interval_ms is None else interval_ms
        assert 0 <= interval_ms <= MAX_INTERVAL, "间隔越界"
        assert 1 <= pulse_ms <= MAX_PULSE, "脉冲越界"
        assert default_level in (0, 1), "默认电平只能为 0 或 1"
        r = self._send(f"AT+RSTCFG={interval_ms},{pin},{pulse_ms},{default_level}", CMD_TIMEOUT)
        if r == "OK":
            self.interval = interval_ms
            self.pulse[pin], self.default_level[pin] = pulse_ms, default_level
            return True
        return False

    def reset(self, order):
        """order: '1'/'2'/'3'/'12'/'21'/'123'/'ALL'/'BOTH' 等"""
        if order == "BOTH":
            order = "12"
        elif order == "ALL":
            order = "123"
        order = str(order)
        assert order and len(order) <= 3 and len(set(order)) == len(order), "非法复位顺序"
        assert all(c in "123" for c in order), "非法复位顺序"
        total = sum(self.pulse[int(c)] for c in order) + (len(order) - 1) * self.interval
        return self._send(f"AT+RST={order}", total / 1000.0 + CMD_TIMEOUT) == "OK"

    def close(self):
        self.ser.close()


def _nums(text, n):
    vals = [int(x) for x in text.split(",")]
    assert len(vals) == n, f"需要 {n} 个数字"
    return vals


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    dev = RstController(argv[1])
    cmd = argv[2].lower()
    try:
        if cmd == "ping":
            print("ping:", dev.ping())
        elif cmd == "query":
            print(dev.query())
        elif cmd == "cfg":
            a, b, c = _nums(argv[3], 3)
            print("configure:", dev.configure(a, b, c))
        elif cmd == "cfgpin":
            pin, pulse, level = _nums(argv[3], 3)
            print(f"configure pin{pin}:", dev.configure_pin(pin, pulse, level))
        elif cmd == "reset":
            argv[3:] or print("缺少复位目标")
            if argv[3:]:
                print(f"reset {argv[3]}:", dev.reset(argv[3]))
        else:
            print(__doc__)
            return 2
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
