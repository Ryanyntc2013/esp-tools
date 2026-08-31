#!/usr/bin/env python3
"""analyze_pcap.py - 解析嗅探器抓包文件并输出统计报告

用法:
  python tools/analyze_pcap.py capture.pcap
  python tools/analyze_pcap.py capture.pcap --top 20
"""
import argparse
import collections
import struct

STYPE = {(0, 8): "Beacon", (0, 4): "ProbeReq", (0, 5): "ProbeResp",
         (0, 11): "Auth", (0, 12): "Deauth", (0, 10): "Disassoc",
         (1, 13): "ACK", (1, 11): "RTS", (1, 12): "CTS"}


def read_ssid(ies):
    i = 0
    while i + 2 <= len(ies):
        tag, elen = ies[i], ies[i + 1]
        if tag == 0:
            raw = ies[i + 2:i + 2 + elen]
            try:
                return raw.decode("utf-8")
            except UnicodeDecodeError:
                return raw.decode("gbk", "replace")   # 国产路由常用 GBK 中文 SSID
        i += 2 + elen
    return ""


def parse(path):
    data = open(path, "rb").read()
    magic, vmaj, vmin, _, _, _, net = struct.unpack("<IHHiIII", data[:24])
    assert magic == 0xA1B2C3D4 and net == 127, "不是本嗅探器的 pcap（µs/radiotap）"
    pkts, off = [], 24
    while off + 16 <= len(data):
        ts_s, ts_u, incl, orig = struct.unpack("<IIII", data[off:off + 16])
        assert incl == orig and 13 <= incl <= 2312, f"坏记录 @{off}"
        pkt = data[off + 16:off + 16 + incl]
        fc = struct.unpack("<H", pkt[13:15])[0]
        # radiotap 占 pkt[0..12]，帧从 13 起；addr2(BSSID)=帧偏移10..16
        pkts.append(dict(
            off=off,
            ts=ts_s + ts_u / 1e6, ts_s=ts_s,
            t=(fc >> 2) & 3, st=(fc >> 4) & 0xF,
            rssi=struct.unpack("b", pkt[12:13])[0],
            ch=(struct.unpack("<H", pkt[8:10])[0] - 2407) // 5,
            bssid=pkt[23:29].hex(":"),
            ssid=read_ssid(pkt[37:]) if ((fc >> 2) & 3) == 0 else "",
        ))
        off += 16 + incl
    return pkts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()

    p = parse(args.pcap)
    dur = max(q["ts"] for q in p) - min(q["ts"] for q in p)
    print(f"== 总览 == {len(p)} 包 / {dur:.1f}s ({len(p)/dur:.1f} pps)")

    bad = [q for q in p if q["ts_s"] > 100000 or q["rssi"] > 0 or not 1 <= q["ch"] <= 13]
    if bad:
        print(f"!! 异常记录 {len(bad)} 条（时间戳/信号/信道越界），疑似流损坏：")
        for q in bad[:5]:
            print(f"   file_off={q['off']} ts_s={q['ts_s']} rssi={q['rssi']} ch={q['ch']}")
        p = [q for q in p if q not in bad]
        dur = max(q["ts"] for q in p) - min(q["ts"] for q in p)
        print(f"   剔除后 {len(p)} 包 / {dur:.1f}s")

    print("-- 帧类型 --")
    for (t, st), c in collections.Counter((q["t"], q["st"]) for q in p).most_common():
        print(f"  type{t} sub{st:<2} {STYPE.get((t, st), '?'):<10} x{c}")

    print("-- 信道分布 --",
          " ".join(f"ch{k}:{v}" for k, v in sorted(collections.Counter(q["ch"] for q in p).items())))

    r = sorted(q["rssi"] for q in p)
    print(f"-- 信号(dBm) -- 最强 {r[-1]}，中位 {r[len(r)//2]}，最弱 {r[0]}")

    aps = {}
    for q in p:
        if q["t"] == 0 and q["st"] in (8, 5):
            e = aps.setdefault(q["bssid"], {"ssid": "", "best": -128, "n": 0, "ch": set()})
            e["ssid"] = e["ssid"] if len(e["ssid"]) > len(q["ssid"]) else q["ssid"]
            e["best"], e["n"] = max(e["best"], q["rssi"]), e["n"] + 1
            e["ch"].add(q["ch"])
    print(f"-- AP 列表：共 {len(aps)} 个 BSSID，按最强信号排序前 {args.top} --")
    for bssid, e in sorted(aps.items(), key=lambda kv: -kv[1]["best"])[:args.top]:
        chans = ",".join(map(str, sorted(e["ch"])))
        print(f"  {e['best']:>4} dBm  ch{chans:<6} {bssid}  {e['ssid'][:30]}")

    hidden = sum(1 for e in aps.values() if not e["ssid"])
    print(f"-- 其中隐藏 SSID 的 AP：{hidden} 个 --")

    print("-- SSID 帧数 Top10 --")
    for s, c in collections.Counter(q["ssid"] for q in p if q["t"] == 0 and q["ssid"]).most_common(10):
        print(f"  {c:>3}  {s[:40]}")


if __name__ == "__main__":
    main()
