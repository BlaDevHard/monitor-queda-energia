#!/usr/bin/env python3
"""Logger em background: grava a serial do Arduino num arquivo, sem fim.

Uso:  python3 logger_serial.py /dev/ttyACM0 9600 /tmp/opencode/zmpt.log
Ctrl-C para parar. O DTR fica em False para nao resetar o Uno.
"""
import sys
import time
import serial

porta = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
dest = sys.argv[3] if len(sys.argv) > 3 else "/tmp/opencode/serial.log"

s = serial.Serial()
s.port = porta
s.baudrate = baud
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
time.sleep(1.5)
s.reset_input_buffer()

buf = b""
with open(dest, "a", buffering=1) as f:
    f.write("\n===== log iniciado %s em %s @%d =====\n"
            % (time.strftime("%Y-%m-%d %H:%M:%S"), porta, baud))
    while True:
        bloco = s.read(256)
        if not bloco:
            continue
        buf += bloco
        while b"\n" in buf:
            linha, buf = buf.split(b"\n", 1)
            f.write(time.strftime("%H:%M:%S ") +
                    linha.decode("utf-8", "replace").rstrip() + "\n")
