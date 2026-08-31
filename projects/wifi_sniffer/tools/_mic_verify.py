#!/usr/bin/env python3
"""_mic_verify.py - 解析四步握手并用正确密码验证 MIC（证明抓包数学上完整可用）

用法:
  python tools/_mic_verify.py mini6207_hs.pcap            # 仅解析握手结构
  python tools/_mic_verify.py mini6207_hs.pcap --verify   # 交互输入密码验证 MIC

原理: PMK = PBKDF2-HMAC-SHA1(密码, SSID, 4096, 32)
      MIC = HMAC-MD5/SHA1(PMK[:16], MIC清零的 EAPOL 帧)   (WPA2-CCMP 用 SHA1)
"""
import getpass
import hashlib
import hmac
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "mini6207_hs.pcap"
do_verify = "--verify" in sys.argv
SNAP_EAPOL = b"\xaa\xaa\x03\x00\x00\x00\x88\x8e"


def load_eapol_frames(path):
    with open(path, "rb") as f:
        data = f.read()
    out = []
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
        eapol = mac[i + 8:]  # 跳过 LLC-SNAP(6B)+ethertype(2B)
        if len(eapol) < 103:
            continue
        t = ts_s + ts_u / 1e6
        out.append((t, mac[10:16], mac[4:10], eapol))
    return out


def classify(eapol):
    # EAPOL-Key 帧体无 EAP code/id/len：desc@4, keyinfo@5:7, keylen@7:9,
    # replay@9:17, nonce@17:49, iv@49:65, rsc@65:73, keyid@73:81, MIC@81:97
    info = struct.unpack(">H", eapol[5:7])[0]
    ack = info & 0x0080
    mic = info & 0x0100
    secure = info & 0x0200
    enc = info & 0x1000
    if ack and not mic:
        return "M1"
    if mic and not ack and not secure and not enc:
        return "M2"
    if ack and mic and secure:
        return "M3"
    if mic and not ack and secure:
        return "M4"
    return f"info=0x{info:04x}"


frames = load_eapol_frames(path)
hs = {}
for t, src, dst, eapol in frames:
    k = classify(eapol)
    hs.setdefault(k, []).append((t, src.hex(":"), eapol))

print("== EAPOL 握手结构 ==")
print(f"  共 {len(frames)} 个 EAPOL 帧; 分类键: {sorted(hs.keys())}")
if frames:
    t0, src0, dst0, e0 = frames[0]
    print(f"  样本: src={src0.hex(':')} eapol[:24]={e0[:24].hex()}")
for k in ("M1", "M2", "M3", "M4"):
    lst = hs.get(k, [])
    print(f"  {k}: {len(lst)} 帧")
    for t, src, eapol in lst[:3]:
        print(f"    t={t:.3f}s from={src}")

m1 = hs.get("M1", [None])[0]
m2_list = hs.get("M2", [])
m4_list = hs.get("M4", [])
if not m2_list and not m4_list:
    print("!! 没有含 MIC 的帧（M2/M4），无法验证")
    sys.exit(1)

def show(tag, e):
    info = struct.unpack(">H", e[5:7])[0]
    ver = info & 0x0007
    body_len = 4 + struct.unpack(">H", e[2:4])[0]
    ok_len = body_len <= len(e)
    print(f"  [{tag}] keyinfo=0x{info:04x} ver={ver}{'(!)' if ver not in (1,2,3) else ''} "
          f"帧长={body_len}{'(!超长)' if not ok_len else ''} SNonce={e[17:49].hex()[:16]}... "
          f"MIC={e[81:97].hex()[:16]}...")

print("\n== 待验证帧 ==")
for t, src, e in m2_list:
    show(f"M2 t={t:.3f}", e)
for t, src, e in m4_list:
    show(f"M4 t={t:.3f}", e)

if not do_verify:
    print("\n用 --verify 参数并输入密码可验证 MIC（模拟破解的数学终点）")
    sys.exit(0)

pw = getpass.getpass("输入该 WiFi 的密码（终端回显关闭）: ").strip()
# SSID 必须精确（区分大小写），PBKDF2 以它为盐
ssid = ""
for a in sys.argv:
    if a.startswith("--ssid="):
        ssid = a.split("=", 1)[1]
if not ssid:
    ssid = input("输入 SSID（区分大小写）: ").strip()

pmk = hashlib.pbkdf2_hmac("sha1", pw.encode(), ssid.encode(), 4096, 32)

ok = False
tested = 0
for tag, lst in (("M2", m2_list), ("M4", m4_list)):
    for t, src, e in lst:
        body_len = 4 + struct.unpack(">H", e[2:4])[0]
        if body_len > len(e) or body_len < 97:
            continue  # 帧长异常，跳过
        captured_mic = e[81:97]
        ez = (e[:81] + b"\x00" * 16 + e[97:])[:body_len]
        for name, h in (("SHA1", hashlib.sha1), ("MD5", hashlib.md5)):
            tested += 1
            calc = hmac.new(pmk[:16], ez, h).digest()[:16]
            if hmac.compare_digest(calc, captured_mic):
                print(f"  ✓ 匹配: [{tag} t={t:.3f}] {name} MIC={calc.hex()}")
                ok = True

print("\n结果: " + (f"MIC 匹配——抓包完整有效，PMK 派生正确（共试 {tested} 组）" if ok
      else f"全部不匹配（共试 {tested} 组）——可能: ①密码/SSID 不精确 ②所有 MIC 帧均有字节损伤"))
