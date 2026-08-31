# -*- coding: utf-8 -*-
"""抓取 ESP32-S3 复位瞬间 ROM 引导器串口输出，判断进入下载模式的原因"""
import serial, time, sys

port = "COM21"
s = serial.Serial(port, 115200, timeout=0.2)
s.reset_input_buffer()
try:
    print("opened", port)
except Exception as e:
    print("open fail:", e); sys.exit(1)
# 读取 6 秒内的全部输出（若已停在下载 stub，这里应能看到 ROM 引导信息）
data = b""
end = time.time() + 6
while time.time() < end:
    try:
        chunk = s.read(1024)
        if chunk:
            data += chunk
    except Exception:
        pass
    time.sleep(0.05)
s.close()
print("received %d bytes" % len(data))
print(data.decode("utf-8", errors="replace"))
