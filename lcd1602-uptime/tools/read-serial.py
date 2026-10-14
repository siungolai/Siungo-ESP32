"""读 COM4 串口日志若干秒并打印。用于无交互终端时采集固件输出。

用法:
    python read-serial.py [秒数]

复位时序说明（ESP32-S3 + 板载原生 USB）:
    自动复位电路用两根控制线经三极管驱动 EN(复位) 与 GPIO0(下载选择):
        RTS -> EN   (拉低 = 复位)
        DTR -> GPIO0(拉低 = 进下载模式)
    所以「复位并进入正常运行」的正确组合是:
        DTR = 0   (GPIO0 保持高，不进下载模式)
        RTS = 1   (拉低 EN 复位) -> 延时 -> RTS = 0 (释放，芯片启动)

    反面教材: DTR=1 同时 RTS=1 会进下载模式，日志停在
    "waiting for download"，程序根本不跑。
"""
import sys
import time

import serial

PORT = "COM4"
BAUD = 115200
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.dtr = False  # GPIO0 保持高：不进下载模式
ser.rts = False
ser.open()

# 复位脉冲：拉低 EN 再释放
ser.rts = True
time.sleep(0.15)
ser.rts = False
time.sleep(0.05)

deadline = time.time() + SECONDS
buf = b""
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk

ser.close()

text = buf.decode("utf-8", errors="replace")
text = text.replace("\r\n", "\n").replace("\r", "\n")
lines = [ln for ln in text.split("\n") if ln.strip()]
for ln in lines[:100]:
    print(ln)
print(f"--- 共 {len(lines)} 行非空输出，采集 {SECONDS:.0f}s ---")
