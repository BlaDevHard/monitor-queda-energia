#!/usr/bin/env python3
"""Captura de dados para calibrar o ZMPT101B (amp -> VRMS).

Le `amp` do gateway ESP32 via HTTP `/estado` (o Arduino alimenta o
ampAtual por UART). Voce anota o V_ref medido no multímetro true-RMS
e o script grava a media e o desvio-padrão de N leituras.

Uso (com o monitor LIGADO na rua e o gateway no WiFi):

    python3 tools/calibracao_captura.py                # IP via /estado local
    python3 tools/calibracao_captura.py --ip 192.168.1.5
    python3 tools/calibracao_captura.py --n 100 --pontos 127 133

O que faz, por ponto:
  1. le N leituras de `amp` (e min/max/saturacao) via GET /estado
  2. pede que voce digite o V_ref do multímetro (Enter p/ pular)
  3. calcula media, desvio-padrão e K_i = V_ref / amp_media
  4. append em data/calibracao_YYYY-MM-DD.csv

Sem variac? Deixe --pontos vazio: entra em modo livre, capturando
quantos pontos voce quiser (nominal + natural). Marque no --nota.
"""
import argparse, json, time, urllib.request, csv, os, statistics, re

def get_estado(ip, porta=80, timeout=4):
    url = f"http://{ip}:{porta}/estado"
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode())

def achar_ip_local():
    """Sem --ip, tenta o IP que o proprio /estado reporta (via hostname)."""
    return "192.168.1.5"   # fallback: IP visto na sessao de 18/10

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=None, help="IP do gateway ESP32")
    ap.add_argument("--porta", type=int, default=80)
    ap.add_argument("--n", type=int, default=100, help="leituras por ponto")
    ap.add_argument("--pontos", nargs="*", type=float, default=None,
                    help="V_ref sugeridos (ex.: 90 100 110 117 120 127 133 135 140)")
    ap.add_argument("--nota", default="", help="ex.: 'sem variac — nominal'")
    args = ap.parse_args()

    ip = args.ip or achar_ip_local()
    print(f"Gateway: http://{ip}:{args.porta}/estado")
    try:
        e = get_estado(ip, args.porta)
        print(f"OK — amp atual={e.get('amp')} energia={e.get('energia')} "
              f"saturado={e.get('sinal_saturado')}\n")
    except Exception as ex:
        print(f"ERRO ao ler /estado: {ex}\nConfira o IP e o gateway ligado.")
        return 1

    data = time.strftime("%Y-%m-%d")
    os.makedirs("data", exist_ok=True)
    caminho = f"data/calibracao_{data}.csv"
    novo = not os.path.exists(caminho)
    f = open(caminho, "a", newline="")
    w = csv.writer(f)
    if novo:
        w.writerow(["timestamp","v_ref","amp_media","sigma_amp","amp_min",
                    "amp_max","saturado","n","k_i","nota"])

    # Pontos: se --pontos, usa a lista; senao modo livre (digitar V_ref, Enter)
    pontos = args.pontos or [None]
    for idx, sugerido in enumerate(pontos, 1):
        print(f"--- Ponto {idx}/{len(pontos)} ---")
        if sugerido is not None:
            print(f"  (V_ref sugerido: {sugerido} V)")
        amps, mins, maxs, sats = [], [], [], []
        coletando = True
        t0 = time.time()
        while len(amps) < args.n:
            try:
                e = get_estado(ip, args.porta)
            except Exception as ex:
                print(f"  leitura falhou ({ex}); retry em 1s")
                time.sleep(1); continue
            amp = e.get("amp")
            if amp is None or amp < 0:
                time.sleep(0.2); continue
            amps.append(amp)
            mins.append(e.get("amp", amp))   # /estado nao expoe min/max
            sats.append(bool(e.get("sinal_saturado")))
            if len(amps) % 20 == 0:
                print(f"  {len(amps)}/{args.n} leituras "
                      f"({time.time()-t0:.0f}s, amp={amp})")
        media = statistics.fmean(amps)
        sigma = statistics.pstdev(amps) if len(amps) > 1 else 0.0
        saturado = any(sats)
        print(f"  MEDIA amp={media:.1f}  sigma={sigma:.2f}  "
              f"saturado={'SIM (invalida o ponto!)' if saturado else 'nao'}")
        if saturado:
            print("  !! Ponto INVALIDO: sinal tocou 0/1023.")

        # V_ref do multímetro
        v_ref = sugerido
        while True:
            bruto = input(f"  V_ref no multímetro true-RMS (V) "
                          f"[{v_ref if v_ref is not None else 'Enter p/ pular'}]: ").strip()
            if bruto == "":
                break
            try:
                v_ref = float(bruto.replace(",", ".")); break
            except ValueError:
                print("  digite um número (ex.: 127.4)")
        k_i = (v_ref / media) if (v_ref and media) else None

        w.writerow([time.strftime("%Y-%m-%dT%H:%M:%S"),
                    v_ref if v_ref is not None else "",
                    round(media,1), round(sigma,2),
                    min(mins) if mins else "", max(maxs) if maxs else "",
                    int(saturado), len(amps),
                    round(k_i,6) if k_i else "", args.nota])
        f.flush()
        print(f"  gravado em {caminho}"
              + (f"  |  K_i = {k_i:.6f} V RMS/conta" if k_i else ""))
        print()

    f.close()
    print("Captura concluida. Proximo passo: analysis/calibracao_regressao.py")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
