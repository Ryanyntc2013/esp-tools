#!/usr/bin/env python3
"""_hdr_count.py - 统计 PCAP 全局头出现次数，判断是否崩溃重启循环
用法: python tools/_hdr_count.py [COM21] [秒数]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM21"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 5

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False

t0 = time.time()
data = b""
while time.time() - t0 < dur:
    d = ser.read(4096)
    if d:
        data += d
ser.close()
hdr = bytes([0xD4, 0xC3, 0xB2, 0xA1])
print(f"{len(data)} bytes; pcap-headers: {data.count(hdr)}")
