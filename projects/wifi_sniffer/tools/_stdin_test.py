#!/usr/bin/env python3
"""_stdin_test.py - 验证 USB CDC 控制台下 'q' 能否切回日志模式

用法: python tools/_stdin_test.py [COM21]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM21"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False
ser.reset_input_buffer()


def read_and_report(tag, dur):
    t0 = time.time()
    data = b""
    while time.time() - t0 < dur:
        d = ser.read(4096)
        if d:
            data += d
    n_ascii = sum(1 for b in data if 32 <= b < 127 or b in (10, 13))
    print(f"[{tag}] {len(data)} bytes, printable={n_ascii}")
    # 打印可打印片段样本
    text = data.decode("utf-8", "replace")
    sample = "".join(c if (32 <= ord(c) < 127) or c in "\r\n" else "." for c in text[:300])
    print(f"[{tag}] sample: {sample}")
    return data


for i in range(4):
    time.sleep(2)
    ser.write(b"q")
    read_and_report(f"after-q{i}", 3)
