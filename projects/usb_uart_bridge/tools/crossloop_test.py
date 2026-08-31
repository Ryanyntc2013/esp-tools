# -*- coding: utf-8 -*-
"""usb_uart_bridge 跨通道环回验证（UART1 TX->UART2 RX, UART2 TX->UART1 RX）

接线:
  GPIO17 (UART1 TX) -> GPIO16 (UART2 RX)
  GPIO15 (UART2 TX) -> GPIO18 (UART1 RX)

预期:
  在 CDC0 发送 'A...'  -> 出现在 CDC1  (CDC0->UART1 TX->UART2 RX->CDC1)
  在 CDC1 发送 'B...'  -> 出现在 CDC0  (CDC1->UART2 TX->UART1 RX->CDC0)

用法: python crossloop_test.py <CDC0> <CDC1>
"""
import sys, time, serial

MSG0 = b"C0->C1 %04d ABCDEFGH\r\n"
MSG1 = b"C1->C0 %04d 0123456789\r\n"

def open_port(port):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = True
    s.rts = True
    time.sleep(0.3)
    s.reset_input_buffer()
    return s

def send_expect(src, dst, msg_template, tag, n=3):
    ok = 0
    for i in range(n):
        dst.reset_input_buffer()
        src.write(msg_template % i)
        src.flush()
        deadline = time.time() + 1.0
        got = b""
        while time.time() < deadline:
            got += dst.read(256)
            if msg_template % i in got:
                ok += 1
                break
        time.sleep(0.05)
    return ok, n

def main():
    if len(sys.argv) != 3:
        print("用法: python crossloop_test.py <CDC0> <CDC1>")
        sys.exit(2)
    cd0, cd1 = open_port(sys.argv[1]), open_port(sys.argv[2])

    print(f"CDC0={sys.argv[1]}  CDC1={sys.argv[2]}")
    print("测试 CDC0 -> CDC1 ...")
    a_ok, a_n = send_expect(cd0, cd1, MSG0, "C0->C1")
    print("测试 CDC1 -> CDC0 ...")
    b_ok, b_n = send_expect(cd1, cd0, MSG1, "C1->C0")

    cd0.close(); cd1.close()

    print("\n===== 交叉环回结果 =====")
    fa = a_ok == a_n
    fb = b_ok == b_n
    print(f"CDC0 -> CDC1: {a_ok}/{a_n} {'PASS' if fa else 'FAIL'}")
    print(f"CDC1 -> CDC0: {b_ok}/{b_n} {'PASS' if fb else 'FAIL'}")
    print("总体:", "PASS" if (fa and fb) else "FAIL")
    sys.exit(0 if (fa and fb) else 1)

if __name__ == "__main__":
    main()
