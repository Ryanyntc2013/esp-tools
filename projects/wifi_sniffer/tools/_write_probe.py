#!/usr/bin/env python3
"""_write_probe.py - 复位后测试 host->device 写是否阻塞
用法: python tools/_write_probe.py [COM21]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM21"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.write_timeout = 3
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False
print("opened", flush=True)

for i in range(3):
    time.sleep(1.5)
    t0 = time.time()
    try:
        n = ser.write(b"q")
        print(f"write#{i}: {n} byte in {time.time()-t0:.2f}s", flush=True)
    except Exception as e:
        print(f"write#{i}: FAIL {e!r} after {time.time()-t0:.2f}s", flush=True)
    d = ser.read(2048)
    ascii_cnt = sum(1 for b in d if 32 <= b < 127 or b in (10, 13))
    print(f"read#{i}: {len(d)} bytes, printable={ascii_cnt}", flush=True)
ser.close()
