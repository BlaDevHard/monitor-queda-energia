#!/usr/bin/env python3
"""Regressão da calibração do ZMPT: ajusta vrms = K * amp (intercepto 0).

Le o CSV gravado por tools/calibracao_captura.py, ajusta K por minimos
quadrados COM intercepto forçado a zero (o plano e o MedTens exigem:
amp=0 implica vrms=0), e mostra erro residual por ponto + incerteza de K.

Uso:
    python3 analysis/calibracao_regressao.py data/calibracao_2026-10-18.csv

Saidas:
    - K (volts RMS por conta pico-a-pico) com desvio-padrão
    - erro relativo de cada ponto (%)
    - grafico amp x V_ref (salvo em data/calibracao_YYYY-MM-DD.png)
    - veredito: <= +-5% em todos os pontos (criterio de aceite PRODIST)
"""
import sys, csv, statistics, os

def main():
    caminho = sys.argv[1] if len(sys.argv) > 1 else None
    if not caminho:
        # pega o CSV mais recente em data/
        cands = sorted([os.path.join("data", f) for f in os.listdir("data")
                        if f.startswith("calibracao_") and f.endswith(".csv")])
        if not cands:
            print("Nenhum CSV em data/. Rode calibracao_captura.py antes.")
            return 1
        caminho = cands[-1]
    print(f"Lendo {caminho}")

    linhas, pontos = [], []
    with open(caminho) as f:
        for row in csv.DictReader(f):
            linhas.append(row)
            try:
                v = float(row["v_ref"]); a = float(row["amp_media"])
                sat = int(row.get("saturado", 0))
            except (ValueError, KeyError):
                continue
            if sat:   # ponto invalido: saturou
                print(f"  IGNORADO (saturado): V_ref={v} amp={a}")
                continue
            if v > 0 and a > 0:
                pontos.append((a, v, row.get("nota", "")))

    if len(pontos) < 1:
        print("Sem pontos validos para regressao.")
        return 1

    # K por minimos quadrados, intercepto FORÇADO A 0:
    #   K = sum(amp_i * v_i) / sum(amp_i^2)
    num = sum(a * v for a, v, _ in pontos)
    den = sum(a * a for a, _, _ in pontos)
    K = num / den if den else 0.0

    print(f"\nPonto(s) valido(s): {len(pontos)}")
    print(f"K = {K:.6f} V RMS / conta p-p   ({K:.4f})")

    # Erro residual por ponto (previsto vs medido)
    print("\n  amp      V_ref    previsto   erro%")
    erros = []
    for a, v, _ in pontos:
        pred = K * a
        erro = (pred - v) / v * 100.0
        erros.append(erro)
        print(f"  {a:6.0f}  {v:6.1f}   {pred:7.1f}   {erro:+6.2f}%")

    # Incerteza de K (desvio-padrao dos K_i individuais, se >1 ponto)
    if len(pontos) > 1:
        ks = [v / a for a, v, _ in pontos]
        dk = statistics.pstdev(ks)
        print(f"\nIncerteza de K (desvio entre pontos): +- {dk:.6f} "
              f"({dk/K*100:.1f}%)")
    else:
        print("\n[!] Apenas 1 ponto — incerteza de K NAO quantificavel. "
              "Calibracao PARCIAL.")

    # Criterio de aceite PRODIST: <= +-5% em todos os pontos
    max_erro = max(abs(e) for e in erros)
    ok = max_erro <= 5.0 and len(pontos) >= 2
    print(f"\nMaior erro: {max_erro:.2f}%  "
          f"-> {'PASSA (<= +-5%)' if ok else 'NAO PASSA / PARCIAL'}")

    # Grafico (matplotlib se disponivel)
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        amps = [a for a, _, _ in pontos]
        vs = [v for _, v, _ in pontos]
        plt.figure(figsize=(6,4))
        plt.scatter(amps, vs, label="medido", zorder=3)
        xs = [min(amps), max(amps)]
        plt.plot(xs, [K*x for x in xs], "r--", label=f"K={K:.4f}")
        plt.xlabel("amp (contagens p-p)"); plt.ylabel("V_ref (V RMS)")
        plt.title("Calibração ZMPT — vrms = K x amp")
        plt.legend(); plt.grid(alpha=0.3)
        png = caminho.replace(".csv", ".png")
        plt.savefig(png, dpi=110, bbox_inches="tight")
        print(f"Grafico salvo em {png}")
    except Exception as ex:
        print(f"(grafico nao gerado: {ex})")

    print(f"\nProximo: definir K={K:.6f} e V_NOM no esp32/include/config.h "
          f"(2.4 do plano).")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
