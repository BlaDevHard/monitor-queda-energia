#!/usr/bin/env python3
"""Observa o power-cycle do ESP32 SEM tocar no aparelho.

Sondeia /estado por HTTP (o ESP32 serve isso mesmo com WiFi de pe).
Nao abre /dev/ttyUSB0 — abrir a porta no CH340 derruba o ESP32.
"""
import json, time, urllib.request, datetime, os

BAIXO = "/tmp/opencode/teste_queda"
LOG = os.path.join(BAIXO, "powercycle.log")
IP = "192.168.1.5"

def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

anterior = None
online = None
f = open(LOG, "a", buffering=1)
f.write(f"\n===== observador power-cycle iniciado {ts()} =====\n")

while True:
    try:
        with urllib.request.urlopen(f"http://{IP}/estado", timeout=3) as r:
            e = json.load(r)
        if online is not True:
            f.write(f"{ts()} | >>> APARELHO ONLINE (de volta) <<<\n")
            online = True
        chave = (e.get("energia"), e.get("em_queda"), e.get("fila"),
                 e.get("slot"), e.get("ota_estado"), e.get("hora_conf"),
                 e.get("amp"))
        if chave != anterior:
            f.write(
                f"{ts()} | uptime={e.get('uptime_s')} desde={e.get('desde')} "
                f"energia={e.get('energia')} em_queda={e.get('em_queda')} "
                f"amp={e.get('amp')} vrms={e.get('vrms')} fila={e.get('fila')} "
                f"hora_conf={e.get('hora_conf')} "
                f"slot={e.get('slot')} ota={e.get('ota_estado')} "
                f"pend={e.get('ota_pendente')} rssi={e.get('wifi_rssi')}\n")
            anterior = chave
    except Exception as ex:
        if online is not False:
            f.write(f"{ts()} | >>> APARELHO OFFLINE: {ex} <<<\n")
            online = False
    time.sleep(0.5)
