#!/usr/bin/env python3
"""Amostrador CONTINUO do /estado, com carimbo de tempo, para parear
com leituras simultaneas do multimetro.

Escreve uma linha por amostra em amostras.tsv:
    timestamp<TAB>amp<TAB>vrms<TAB>freq<TAB>energia<TAB>em_queda
Uso:  nohup python3 continuo.py > /dev/null 2>&1 &
Depois: grep o timestamp mais proximo da leitura do multimetro.
"""
import json, time, urllib.request, datetime, os

IP = "192.168.1.5"
SAIDA = "/tmp/opencode/teste_queda/amostras.tsv"

f = open(SAIDA, "a", buffering=1)
f.write(f"\n# continuo iniciado {datetime.datetime.now().isoformat()}\n")

while True:
    try:
        with urllib.request.urlopen(f"http://{IP}/estado", timeout=3) as r:
            d = json.load(r)
        f.write(
            f"{datetime.datetime.now().isoformat(timespec='milliseconds')}\t"
            f"{d.get('amp')}\t{d.get('vrms')}\t{d.get('freq_hz')}\t"
            f"{d.get('energia')}\t{d.get('em_queda')}\n")
    except Exception:
        f.write(f"{datetime.datetime.now().isoformat(timespec='milliseconds')}\tERRO\n")
    time.sleep(0.3)
