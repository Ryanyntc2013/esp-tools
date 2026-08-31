# -*- coding: utf-8 -*-
"""usb_uart_bridge 双通道验证脚本（PC 端）

用法: python bridge_test.py <CDC0的COM口> <CDC1的COM口>
功能:
  1. 同时打开两个虚拟 COM 口（DTR/RTS 置位）
  2. 各口发送测试报文，校验环回（需对端把 TX-RX 短接）或对端回显
  3. 校验波特率跟随：改 9600 后发报文（短接环回时校验字节完整性）
  4. 打印每通道收发统计与 PASS/FAIL
"""
import sys, time, argparse
import serial

MSG = b"BRIDGE-CH%d-%04d-0123456789ABCDEF\r\n"

def open_port(port):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = True   # 触发固件 s_usb_opened，开始向 USB 转发
    s.rts = True
    time.sleep(0.3)
    s.reset_input_buffer()
    return s

def roundtrip(s, ch, baud, n=3):
    ok = 0
    for i in range(n):
        s.reset_input_buffer()
        s.write(MSG % (ch, i))
        s.flush()
        deadline = time.time() + 1.0
        got = b""
        while time.time() < deadline:
            got += s.read(256)
            if MSG % (ch, i) in got:
                ok += 1
                break
        time.sleep(0.05)
    return ok, n

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port0")
    ap.add_argument("port1")
    ap.add_argument("--loopback", action="store_true",
                    help="两路 UART 的 TX/RX 均已短接（环回校验）")
    args = ap.parse_args()

    results = {}
    for name, port in (("CDC0", args.port0), ("CDC1", args.port1)):
        s = open_port(port)
        ch = 0 if name == "CDC0" else 1
        # 115200 环回
        ok, n = roundtrip(s, ch, 115200)
        results[name] = {"115200": (ok, n)}
        # 波特率跟随校验（环回模式下 9600 仍应完整收到）
        if args.loopback:
            s.baudrate = 9600
            time.sleep(0.2)
            ok9, n9 = roundtrip(s, ch, 9600)
            results[name]["9600"] = (ok9, n9)
        s.close()

    print("\n===== 验证结果 =====")
    all_pass = True
    for name, r in results.items():
        for baud, (ok, n) in r.items():
            status = "PASS" if ok == n else "FAIL"
            if ok != n:
                all_pass = False
            print(f"{name} @ {baud}: {ok}/{n} {status}")
    print("总体:", "PASS" if all_pass else "FAIL")
    sys.exit(0 if all_pass else 1)

if __name__ == "__main__":
    main()
