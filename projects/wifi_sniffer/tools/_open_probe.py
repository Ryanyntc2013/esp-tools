#!/usr/bin/env python3
"""_open_probe.py - 逐步打点定位 COM21 打开卡死的位置"""
import sys
import time

print("step1: import serial", flush=True)
import serial

print("step2: construct", flush=True)
ser = serial.Serial()
ser.port = "COM21"
ser.baudrate = 115200
ser.timeout = 1

print("step3: open() call", flush=True)
t0 = time.time()
ser.open()
print(f"step4: opened in {time.time()-t0:.2f}s", flush=True)

ser.dtr = False
ser.rts = False
print("step5: lines set", flush=True)

t0 = time.time()
d = ser.read(64)
print(f"step6: read {len(d)} bytes in {time.time()-t0:.2f}s: {d[:32].hex()}", flush=True)
ser.close()
print("step7: closed", flush=True)
