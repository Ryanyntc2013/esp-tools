#!/usr/bin/env python3
"""serial_pcap.py - ESP32 WiFi 嗅探器 PC 端工具

用法:
  python tools/serial_pcap.py COM20 capture.pcap          # 切到 PCAP 模式并抓包存盘 (921600)
  python tools/serial_pcap.py COM20 -b 115200 capture.pcap
  python tools/serial_pcap.py COM20 --log                 # 切回日志模式后退出
停止抓包: Ctrl+C，生成的 capture.pcap 用 Wireshark 直接打开。

依赖: pip install pyserial
"""
import sys
import time
import argparse

import serial

PCAP_MAGIC = bytes([0xD4, 0xC3, 0xB2, 0xA1])
RTAP_SIG = bytes([0x00, 0x00, 0x0D, 0x00])   # radiotap rev=0 pad=0 hdrlen=13


def main():
    ap = argparse.ArgumentParser(description="ESP32 sniffer serial -> pcap")
    ap.add_argument("port", help="串口号, 如 COM20")
    ap.add_argument("outfile", nargs="?", help="输出 .pcap 文件路径")
    ap.add_argument("-b", "--baud", type=int, default=921600,
                    help="波特率，须与固件控制台一致（默认 921600）")
    ap.add_argument("--log", action="store_true", help="发 'q' 切回日志模式后退出")
    ap.add_argument("-s", "--seconds", type=float, default=0,
                    help="抓包时长(秒)，0 表示持续到 Ctrl+C")
    ap.add_argument("-c", "--channel", type=int, default=0,
                    help="锁定信道 1-13 后再抓（抓登录/握手必须锁信道），0 表示保持跳频")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 1
    ser.dtr = False   # 不拉 DTR/RTS，避免触发板子复位进入下载模式
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    time.sleep(0.2)

    if args.log:
        ser.write(b"q")
        print("sent 'q', device switched back to log mode in place")
        return

    # 状态感知式进入 PCAP：可能已经在流（NVS 记忆了上次模式）
    def find_magic():
        data = ser.read(512)
        i = data.find(PCAP_MAGIC)
        return data[i:] if i >= 0 else None

    synced_data = None
    if not args.seconds or args.seconds > 0:
        pass
    print("checking current device mode...")
    t0 = time.time()
    while time.time() - t0 < 4:
        chunk = find_magic()
        if chunk:
            synced_data = chunk
            print("device already streaming pcap")
            break

    if synced_data is None:
        for attempt in ("p", "q->p"):
            if attempt == "p":
                ser.write(b"p")
                print("sent 'p', waiting for pcap header...")
                wait = 6
            else:
                ser.write(b"q")          # 先回日志模式，确保拿到干净的全局头
                time.sleep(3)
                ser.reset_input_buffer()
                ser.write(b"p")
                print("sent 'q'+'p', waiting for pcap header...")
                wait = 8
            t0 = time.time()
            while time.time() - t0 < wait:
                chunk = find_magic()
                if chunk:
                    synced_data = chunk
                    break
            if synced_data:
                break
        if not synced_data:
            print("ERROR: no pcap header seen; check baud (-b) matches firmware console")
            return

    if args.channel:
        ser.write(f"{args.channel}\n".encode())   # 此时已处于 PCAP 模式，固件不回显
        time.sleep(0.5)
        print(f"locked channel {args.channel}")

    deadline = time.time() + args.seconds if args.seconds > 0 else None
    synced, total = True, 0
    with open(args.outfile, "wb") as f:
        try:
            buf = synced_data
            while True:
                if deadline and time.time() >= deadline:
                    break
                if buf:
                    data, buf = buf, b""
                else:
                    # 尽量大口吞掉积压字节，避免驱动缓冲溢出丢数据
                    data = ser.read(ser.in_waiting or 4096)
                    if not data:
                        continue
                total += len(data)
                f.write(data)   # 不逐块 flush，靠关闭时统一落盘保吞吐
        except KeyboardInterrupt:
            pass
    print(f"saved {total} bytes -> {args.outfile}")
    kept, dropped = sanitize(args.outfile)
    if dropped:
        print(f"sanitize: kept {kept} packets, resynced/dropped {dropped} bytes")
    else:
        print(f"sanitize: {kept} packets, stream clean")
    print(f"open it with: wireshark {args.outfile}")


def _hdr_ok(data, off, last_ts):
    """完整合法性：长度自洽 + radiotap 特征 + 时间戳/信道/信号合理且单调"""
    if off + 16 > len(data):
        return False
    ts_s, ts_u = int.from_bytes(data[off:off + 4], "little"), int.from_bytes(data[off + 4:off + 8], "little")
    l1 = int.from_bytes(data[off + 8:off + 12], "little")
    l2 = int.from_bytes(data[off + 12:off + 16], "little")
    if l1 != l2 or not 13 <= l1 <= 2312 or off + 16 + l1 > len(data):
        return False
    if data[off + 16:off + 20] != RTAP_SIG:
        return False
    pkt = data[off + 16:off + 16 + l1]
    if len(pkt) < 13:
        return False
    ch = (int.from_bytes(pkt[8:10], "little") - 2407) // 5
    rssi = pkt[12] - 256 if pkt[12] > 127 else pkt[12]
    ts = ts_s + ts_u / 1e6
    return (ts_u < 1000000 and ts_s < 1000000 and 1 <= ch <= 13
            and -128 <= rssi < 0 and ts >= last_ts - 0.5)


def sanitize(path):
    """扫描 pcap，遇到非法记录头时向前重同步；返回 (保留数, 丢弃字节)"""
    data = open(path, "rb").read()
    if len(data) < 24 or data[:4] != PCAP_MAGIC:
        return 0, 0
    out = bytearray(data[:24])
    off, kept, dropped, last_ts = 24, 0, 0, 0.0
    while off + 16 <= len(data):
        if _hdr_ok(data, off, last_ts):
            incl = int.from_bytes(data[off + 8:off + 12], "little")
            out += data[off:off + 16 + incl]
            last_ts = int.from_bytes(data[off:off + 4], "little") + \
                int.from_bytes(data[off + 4:off + 8], "little") / 1e6
            off += 16 + incl
            kept += 1
            continue
        found = False
        for scan in range(off + 1, min(len(data) - 20, off + 4096)):
            if _hdr_ok(data, scan, last_ts):
                dropped += scan - off
                off = scan
                found = True
                break
        if not found:
            dropped += len(data) - off
            break
    with open(path, "wb") as f:
        f.write(out)
    return kept, dropped


if __name__ == "__main__":
    main()
