#!/usr/bin/env python3
"""_hs_check.py - 检查 pcap 中的登录握手帧（Authentication/Association/EAPOL）
用法: python tools/_hs_check.py login_ch3.pcap
"""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "login_ch3.pcap"

MGMT_NAMES = {0: "AssocReq", 1: "AssocResp", 2: "ReassocReq", 3: "ReassocResp",
              4: "ProbeReq", 5: "ProbeResp", 8: "Beacon", 10: "Disassoc",
              11: "Auth", 12: "Deauth"}

with open(path, "rb") as f:
    data = f.read()

off = 24  # global header
hits = {"auth": [], "assoc": [], "eapol": [], "disassoc": [], "deauth": []}
n = 0
while off + 16 <= len(data):
    ts_s, ts_u, incl, orig = struct.unpack("<IIII", data[off:off + 16])
    off += 16
    pkt = data[off:off + incl]
    off += incl
    n += 1
    if len(pkt) < 16:  # radiotap 至少 13B + 帧头
        continue
    rt_len = pkt[2] | (pkt[3] << 8)
    mac = pkt[rt_len:]
    if len(mac) < 24:
        continue
    fc = mac[0] | (mac[1] << 8)
    ftype = (fc >> 2) & 0x3
    subtype = (fc >> 4) & 0xF
    addr2 = mac[10:16].hex(":")
    addr3 = mac[16:22].hex(":")
    if ftype == 0 and subtype in (0, 1, 2, 3, 10, 11, 12):
        name = MGMT_NAMES[subtype]
        key = {"AssocReq": "assoc", "AssocResp": "assoc", "ReassocReq": "assoc",
               "ReassocResp": "assoc", "Auth": "auth", "Disassoc": "disassoc",
               "Deauth": "deauth"}[name]
        # Auth 帧固定参数前 2 字节是认证算法: 0=Open, 1=Shared, 3=SAE
        algo = ""
        if subtype == 11 and len(mac) >= 26:
            a = struct.unpack("<H", mac[24:26])[0]
            algo = {0: "open", 1: "shared", 2: "FT", 3: "SAE"}.get(a, f"algo{a}")
        hits[key].append((ts_s + ts_u / 1e6, f"{name}({algo})" if algo else name, addr2, addr3))
    elif ftype == 2:
        # 数据帧：任意偏移匹配 LLC-SNAP + EAPOL ethertype（容忍 QoS/HT 控制字段长度差异）
        if b"\xaa\xaa\x03\x00\x00\x00\x88\x8e" in mac:
            hits["eapol"].append((ts_s + ts_u / 1e6, "EAPOL", addr2, addr3))

for k, v in hits.items():
    print(f"== {k}: {len(v)} 帧")
    for t, name, a2, a3 in v[:12]:
        print(f"   t={t:.3f}s {name} from={a2} bssid={a3}")
print(f"共解析 {n} 包")
