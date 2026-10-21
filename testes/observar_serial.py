#!/usr/bin/env python3
"""Observa o teste de queda: serial do ESP32 + /estado, com carimbo de tempo."""
import json, time, urllib.request, datetime, sys, os

BAIXO = "/tmp/opencode/teste_queda"
SERIAL_LOG = os.path.join(BAIXO, "serial.log")
ESTADO_LOG = os.path.join(BAIXO, "estado.log")
IP = "192.168.1.5"

def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

def estado():
    with urllib.request.urlopen(f"http://{IP}/estado", timeout=4) as r:
        return json.load(r)

# ---------- 1. serial ----------
# ⚠️ NAO usar setRTS(True) aqui: no CH340 o RTS e o pino EN do ESP32.
# Aquela linha de "auto-reset" (que serve para FLASHAR) reinicia o
# aparelho — no 18/10 derrubou o monitor em producao no meio da
# validacao de campo. Para observar, abrir a porta e desligar os dois:
import serial  # pyserial
s = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.3)
s.setDTR(False)
s.setRTS(False)

sf = open(SERIAL_LOG, "a", buffering=1)
sf.write(f"\n===== observacao iniciada {ts()} =====\n")

# ---------- 2. estado ----------
ef = open(ESTADO_LOG, "a", buffering=1)
anterior = None
t0 = time.time()
prox = time.time()

while True:
    # serial (nao-bloqueante)
    d = s.read(4096)
    if d:
        for linha in d.decode("utf-8", "replace").splitlines():
            if linha.strip():
                sf.write(f"{ts()} | {linha}\n")

    # /estado, 1x por segundo
    if time.time() >= prox:
        prox = time.time() + 1.0
        try:
            e = estado()
            chave = (e.get("energia"), e.get("em_queda"), e.get("fila"),
                     e.get("amp"), e.get("ultima_queda_s"))
            if chave != anterior:
                ef.write(
                    f"{ts()} | energia={e.get('energia')} "
                    f"em_queda={e.get('em_queda')} amp={e.get('amp')} "
                    f"vrms={e.get('vrms')} fila={e.get('fila')} "
                    f"ultima_queda_s={e.get('ultima_queda_s')} "
                    f"duracao_s={e.get('duracao_s')} "
                    f"desde={e.get('desde')} uptime={e.get('uptime_s')}\n")
                anterior = chave
        except Exception as ex:
            ef.write(f"{ts()} | ERRO /estado: {ex}\n")
