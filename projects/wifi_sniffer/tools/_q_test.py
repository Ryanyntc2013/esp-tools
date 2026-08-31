#!/usr/bin/env python3
"""_q_test.py - 单次验证 'q' 能否切回日志模式
用法: python tools/_q_test.py [COM21]
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

# 打开端口可能触发 DTR 瞬态复位；等启动完成后反复发 'q' 直到看到日志文本
print("waiting for boot...", flush=True)
t0 = time.time()
boot_done = False
data = b""
sent = 0
while time.time() - t0 < 10:
    if time.time() - t0 > 2.5 and sent < 5:
        ser.write(b"q")   # 日志模式下 'q' 无副作用
        sent += 1
    d = ser.read(4096)
    if d:
        data += d
        if b"log mode" in data:
            boot_done = True
            break
ser.close()
print(f"sent {sent} 'q'; log mode reached: {boot_done}")
print("HEAD:", repr(data[:600]))
text = data.decode("utf-8", "replace")
idx = text.find("log mode")
if idx >= 0:
    print("TAIL:", "".join(c if 32 <= ord(c) < 127 or c in "\r\n" else "." for c in text[max(0, idx - 100): idx + 300]))
else:
    print(f"{len(data)} bytes, no 'log mode' seen")
