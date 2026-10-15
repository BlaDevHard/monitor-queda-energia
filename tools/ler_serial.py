#!/usr/bin/env python3
"""Le a serial do Arduino (9600) por N segundos e imprime tudo.

Nao mexe em DTR/RTS de proposito — pyserial aciona DTR ao abrir, o que
reseta o Uno; por isso abrimos com dsrdtr e esperamos o boot passar.
"""
import sys
import time
import serial

porta = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
dur = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0

s = serial.Serial()
s.port = porta
s.baudrate = baud
s.timeout = 0.2
s.dtr = False      # evita reset ao abrir
s.rts = False
s.open()

# Se o DTR resetou o Uno, ele leva ~1-2s pra voltar e reimprimir.
# Nesse periodo o buffer e vazio — entao nao gravamos nada ainda.
time.sleep(1.5)
s.reset_input_buffer()

fim = time.time() + dur
buf = b""
while time.time() < fim:
    bloco = s.read(256)
    if bloco:
        buf += bloco
        while b"\n" in buf:
            linha, buf = buf.split(b"\n", 1)
            print(linha.decode("utf-8", "replace").rstrip())
s.close()
if buf:
    print(buf.decode("utf-8", "replace").rstrip())
