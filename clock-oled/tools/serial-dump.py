"""一次性串口抓取：读 N 秒，把收到的字节原样打印出来。

pio device monitor 要求交互式终端，不能在管道/重定向下跑，
所以脚本化验收时直接读串口。用 pio 自带的 pyserial，避免额外装包。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0

# 先置 DTR/RTS 触发板子复位，这样能从头看到启动日志（含 setup() 输出）。
ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.dtr = False
ser.rts = True   # RTS 拉高 -> EN 低 -> 复位
ser.open()
time.sleep(0.15)
ser.rts = False  # 释放复位，板子开始跑
time.sleep(0.05)
ser.reset_input_buffer()

deadline = time.time() + SECONDS
buf = bytearray()
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf.extend(chunk)
        # 边收边吐，避免最后一次性输出被截断
        sys.stdout.write(chunk.decode("utf-8", errors="replace"))
        sys.stdout.flush()
ser.close()
sys.stdout.write("\n--- EOF ({} bytes) ---\n".format(len(buf)))
