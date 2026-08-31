#!/usr/bin/env python3
"""_dump_eapol.py - dump EAPOL 帧原始字节，人工对齐偏移"""
import struct
import sys

SNAP_EAPOL = b"\xaa\xaa\x03\x00\x00\x00\x88\x8e"

with open(sys.argv[1] if len(sys.argv) > 1 else "mini6207_hs.pcap", "rb") as f:
    data = f.read()

off = 24
while off + 16 <= len(data):
    ts_s, ts_u, incl, _ = struct.unpack("<IIII", data[off:off + 16])
    off += 16
    pkt = data[off:off + incl]
    off += incl
    if len(pkt) < 30:
        continue
    rt_len = pkt[2] | (pkt[3] << 8)
    mac = pkt[rt_len:]
    i = mac.find(SNAP_EAPOL)
    if i < 0:
        continue
    t = ts_s + ts_u / 1e6
    e = mac[i + 8:]
    print(f"== t={t:.3f} from={mac[10:16].hex(':')} len={len(e)}")
    print(f"   [0:21]  {e[0:21].hex(' ')}")
    print(f"   [21:53] {e[21:53].hex(' ')}")
    print(f"   [81:97] {e[81:97].hex(' ')}")
