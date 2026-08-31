#!/usr/bin/env python3
"""_probe_baud.py - 复位设备后在不同波特率下读启动横幅，判断固件实际控制台速率"""
import time
import serial

PORT = "COM20"
for baud in (74880, 921600, 115200):
    ser = serial.Serial(port=PORT, baudrate=baud, timeout=0.5)
    ser.dtr = False
    ser.rts = True          # 拉低 EN 复位
    time.sleep(0.1)
    ser.rts = False         # 释放，芯片启动打印横幅
    chunks = bytearray()
    t_end = time.time() + 2.5
    while time.time() < t_end:
        chunks += ser.read(2048)
    ser.close()
    data = bytes(chunks)
    open(f"E:/test/esp32-s3/probe_{baud}.bin", "wb").write(data)
    txt = "".join(chr(c) if 32 <= c < 127 or c in (10, 13) else "." for c in data[:120])
    has_boot = b"boot:" in data or b"ESP-ROM" in data
    has_magic = bytes([0xD4, 0xC3, 0xB2, 0xA1]) in data
    print(f"@{baud:>7}: {len(data):>5}B boot_text={has_boot} pcap_magic={has_magic} {txt!r}")
