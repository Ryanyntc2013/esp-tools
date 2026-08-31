#!/usr/bin/env python3
"""_raw_debug.py - 抓取原始串口字节流用于诊断（不做任何净化）"""
import sys
import time
import serial

port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
ser = serial.Serial(port=port, baudrate=921600, timeout=1)
ser.dtr = ser.rts = False
time.sleep(0.2)
ser.write(b"q"); time.sleep(3); ser.reset_input_buffer(); ser.write(b"p")
buf = bytearray()
deadline = time.time() + secs
while time.time() < deadline:
    buf += ser.read(8192)
open(out, "wb").write(buf)
print(f"raw {len(buf)} bytes -> {out}")
# 统计可打印比例与长 ASCII 段
pr = sum(1 for c in buf if 32 <= c < 127 or c in (9, 10, 13))
print(f"printable ratio: {pr/len(buf)*100:.1f}%")
runs, start = [], None
for i, c in enumerate(buf + b"\x00"):
    if 32 <= c < 127 or c in (9, 10, 13):
        if start is None:
            start = i
    else:
        if start is not None and i - start > 60:
            runs.append((start, i))
        start = None
print(f"long ascii runs(>60): {len(runs)}")
for s, e in runs[:6]:
    print(f"--- @{s} ({e-s}B): {buf[s:min(e,s+160)].decode('ascii','replace')!r}")

# ---- magic 之后的完整性与文本混入检查 ----
import struct
MAGIC = bytes([0xD4, 0xC3, 0xB2, 0xA1])
i = buf.find(MAGIC)
if i < 0:
    print("NO MAGIC FOUND"); sys.exit(1)
tail = buf[i:]
runs2, start2 = [], None
for j, c in enumerate(tail + b"\x00"):
    if 32 <= c < 127 or c in (9, 10, 13):
        if start2 is None:
            start2 = j
    else:
        if start2 is not None and j - start2 > 60:
            runs2.append((start2, j))
        start2 = None
print(f"[post-magic {len(tail)}B] ascii runs(>60): {len(runs2)}")
for s, e in runs2[:5]:
    print(f"+++ +{s}: {tail[s:min(e,s+140)].decode('ascii','replace')!r}")
off, n, badsz = i + 24, 0, 0
while off + 16 <= len(buf):
    ts_s, ts_u, l1, l2 = struct.unpack("<IIII", buf[off:off + 16])
    ok = (l1 == l2 and 13 <= l1 <= 2312 and off + 16 + l1 <= len(buf)
          and buf[off + 16:off + 20] == bytes([0, 0, 13, 0])
          and ts_u < 1000000 and ts_s < 100000)
    if ok:
        n += 1
        off += 16 + l1
    else:
        badsz += 1
        off += 1
print(f"valid records={n}, rejected byte-steps={badsz}")
