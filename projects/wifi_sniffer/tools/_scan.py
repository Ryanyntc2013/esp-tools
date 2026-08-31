#!/usr/bin/env python3
"""_scan.py - 读取嗅探器日志模式输出，列出附近 AP（确认目标信道用）

用法: python tools/_scan.py [COM21] [秒数, 默认30]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM21"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 30

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False  # USB-Serial/JTAG 拉 DTR/RTS 会把芯片打进下载模式
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False
ser.reset_input_buffer()
time.sleep(2.0)   # 等固件 stdin 任务就绪再发命令，过早发送会被丢弃
ser.write(b"q")
t0 = time.time()
while time.time() - t0 < dur:
    d = ser.read(4096)
    if d:
        sys.stdout.write(d.decode("utf-8", "replace"))
        sys.stdout.flush()
