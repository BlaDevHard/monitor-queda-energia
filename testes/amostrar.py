#!/usr/bin/env python3
"""Amostra o /estado por N segundos e devolve as estatisticas do amp.

O CSV da calibracao quer amp_media, sigma_amp, amp_min, amp_max e n —
uma leitura isolada nao serve, porque o amp do ZMPT quica.
Uso:  python3 amostrar.py [segundos]     (padrao 20)
"""
import json, sys, time, urllib.request, datetime, statistics

IP = "192.168.1.5"
SEG = int(sys.argv[1]) if len(sys.argv) > 1 else 20

amps, vrms, freqs = [], [], []
t0 = time.time()
while time.time() - t0 < SEG:
    try:
        with urllib.request.urlopen(f"http://{IP}/estado", timeout=3) as r:
            d = json.load(r)
        a = d.get("amp", -1)
        if a > 0:                      # descarta o -1 do boot
            amps.append(a)
            vrms.append(d.get("vrms", 0))
            freqs.append(d.get("freq_hz", 0))
    except Exception:
        pass
    time.sleep(0.4)

if not amps:
    print("ERRO: nenhuma leitura valida")
    sys.exit(1)

media = statistics.mean(amps)
sigma = statistics.pstdev(amps) if len(amps) > 1 else 0.0
n = len(amps)

print(f"ts          = {datetime.datetime.now().strftime('%Y-%m-%dT%H:%M:%S')}")
print(f"amp_media   = {media:.1f}")
print(f"sigma_amp   = {sigma:.3f}")
print(f"amp_min     = {min(amps)}")
print(f"amp_max     = {max(amps)}")
print(f"n           = {n}")
print(f"vrms_media  = {statistics.mean(vrms):.1f} V")
print(f"freq_media  = {statistics.mean(freqs):.1f} Hz")
print(f"k_i         = {statistics.mean(vrms)/media:.6f}   (v_ref/amp — ainda sem o multimetro)")
print()
print("AMP bruto:", " ".join(str(a) for a in amps))
