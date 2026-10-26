#!/usr/bin/env python3
"""
usb_uart_bridge 复位控制测试工具（6 路：GPIO4/5/6/7/8/9）

用法:
  python reset_test.py COM23 ping
  python reset_test.py COM23 query
  python reset_test.py COM23 cfg 100,50,0            # 六路统一配置
  python reset_test.py COM23 cfgpin 6,500,0          # 只配置 6 号脚（GPIO9）
  python reset_test.py COM23 mode                    # 查询空闲模式（HIGHZ/DRIVE）
  python reset_test.py COM23 mode 1                  # 设为高阻空闲（默认）
  python reset_test.py COM23 mode 0                  # 设为推挽空闲（持续输出默认电平）
  python reset_test.py COM23 lvl                     # 读取全部六脚当前电平
  python reset_test.py COM23 lvl 3                   # 读取 3 号脚（GPIO6）电平
  python reset_test.py COM23 reset 4                 # 只复位 GPIO7
  python reset_test.py COM23 reset 456               # 依次 GPIO7->8->9
  python reset_test.py COM23 reset ALL               # 六路全部依次复位

依赖: pip install pyserial
协议说明: docs/at-rst-control.md
"""
import sys
import time

import serial

CMD_TIMEOUT = 0.5
MAX_INTERVAL = 60000
MAX_PULSE = 60000
# 逻辑脚号 -> GPIO
RESET_PINS = {1: 4, 2: 5, 3: 6, 4: 7, 5: 8, 6: 9}


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
            # IDLE 是字符串(HIGHZ/DRIVE)，其余字段是整数
            out[k] = v if k == "IDLE" else int(v)
        self.interval = out["INTERVAL"]
        self.idle_mode = out.get("IDLE")
        for p in RESET_PINS:
            self.pulse[p] = out[f"PULSE{p}"]
            self.default_level[p] = out[f"DEFAULT{p}"]
        return out

    def configure(self, interval_ms, pulse_ms, default_level):
        """统一配置全部六路参数"""
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
        assert pin in RESET_PINS, "脚号只能是 1..6"
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

    def query_mode(self):
        """返回 'HIGHZ' / 'DRIVE' / None"""
        r = self._send("AT+RSTMODE?", CMD_TIMEOUT, expect=("+RSTMODE:",))
        if not r or not r.startswith("+RSTMODE:"):
            return None
        return r[len("+RSTMODE:"):]

    def configure_mode(self, high_z=True):
        """True=空闲高阻（默认），False=空闲推挽输出"""
        return self._send(f"AT+RSTMODE={1 if high_z else 0}", CMD_TIMEOUT) == "OK"

    def read_levels(self):
        """读取全部六脚电平，返回 {脚号: 0/1}"""
        r = self._send("AT+RSTLVL", CMD_TIMEOUT, expect=("+RSTLVL:",))
        if not r or not r.startswith("+RSTLVL:"):
            return None
        out = {}
        for kv in r[len("+RSTLVL:"):].split(","):
            k, _, v = kv.partition("=")
            if k.startswith("P"):
                out[int(k[1:])] = int(v)
        return out

    def read_level(self, pin):
        """读取单个脚电平，返回 0/1 或 None"""
        assert pin in RESET_PINS, "脚号只能是 1..6"
        r = self._send(f"AT+RSTLVL={pin}", CMD_TIMEOUT, expect=("+RSTLVL:",))
        if not r:
            return None
        return int(r.rsplit("=", 1)[1])

    def reset(self, order):
        """order: 1..6 的不重复排列，如 '1'/'12'/'456'/'123456'；别名 'ALL'/'BOTH'"""
        if order == "BOTH":
            order = "12"
        elif order == "ALL":
            order = "123456"
        order = str(order)
        assert 0 < len(order) <= 6 and len(set(order)) == len(order), "非法复位顺序"
        assert all(c in "123456" for c in order), "非法复位顺序"
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
        elif cmd == "mode":
            if len(argv) > 3:
                print("set mode high_z:", dev.configure_mode(argv[3].lower() in ("1", "highz", "high_z")))
            else:
                print("mode:", dev.query_mode())
        elif cmd == "lvl":
            if len(argv) > 3:
                print(f"level P{argv[3]}:", dev.read_level(int(argv[3])))
            else:
                print("levels:", dev.read_levels())
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
