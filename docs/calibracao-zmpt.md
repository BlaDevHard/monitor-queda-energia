# Calibração do ZMPT — conversão `amp` → VRMS

> **Status: 18/10/2026 — PARCIAL MELHORADA (5 pontos, faixa estreita).**
> Linearidade verificada, incerteza quantificada (±0,60%), maior erro
> residual 1,01% (passa ≤ ±5%). **Ainda NÃO cobre a faixa PRODIST**
> (90–150 V) — para apuração DRP/DRC do MedTens ainda falta o **variac**.
>
> Firmware carrega `K_VRMS = 0.2976` (valor do **ponto 1**, mantido por
> decisão — ver §6). A regressão final deu **K = 0,2987**.

---

## 1. O que se calibra e por quê

O Arduino não mede tensão: mede a **amplitude pico-a-pico** do sinal do
ZMPT101B em contagens do ADC (`amp`, exemplo `#ZMPT amp=417 [min=302
max=719]`). Para publicar **volts** é preciso um fator:

```
vrms = K_VRMS × amp
```

Um **único** `K` (volts RMS por conta pico-a-pico) absorve, de uma vez:

| Grandeza física | O que é |
|---|---|
| `Vref`/`AVcc` | tensão de referência do ADC (≈5 V, varia USB vs power bank) |
| razão do ZMPT | ganho/isolamento do transformador de senoide |
| conversão p→RMS | ÷2√2 para senoide pura (60 Hz) |

Essa é a diferença para o **MedTens**, de onde vem a metodologia: lá se
calibra um `calibration_factor` sobre **VRMS**; aqui se calibra `K` sobre
**`amp`**. O alvo é o mesmo — o fator só é mais "gordo".

> **Metodologia importada de:** `medtens-docs/docs/calibracao.md`
> (repo `BlaDevHard/medtens-docs`). Plano: [`plano-ota-qualidade.md`](plano-ota-qualidade.md)
> → Prioridade 2.

---

## 2. Ferramentas e condições (reprodução)

| Item | Valor |
|---|---|
| Multímetro | **ANENG M118A**, **True RMS**, `±(0,5%+3d)` (≈±0,9 V a 127 V) |
| Alimentação | **power bank** (igual campo) — **não** USB do PC, para valer o `AVcc` real |
| Ponto de medição | **terminais de ENTRADA do ZMPT** (rede) |
| Variac | ❌ **ausente** — é o que limita a faixa |
| Leituras por ponto | 37–58 leituras de `amp` em 60 s (média + σ) |
| Ferramentas | `tools/calibracao_captura.py` (captura), `analysis/calibracao_regressao.py` (regressão) |

**Regra de ouro:** `min`/`max` do `amp` **não podem** tocar 0/1023 —
saturação invalida o ponto. (Nenhum dos 5 pontos saturou.)

---

## 3. Os 5 pontos — `data/calibracao_2026-10-18.csv`

| # | Hora | `V_ref` (V) | `amp` médio | σ | `K_i` | Nota |
|---|---|---|---|---|---|---|
| 1 | 14:18 | 123,90 | 416,30 | 2,45 | 0,297651 | fallback 1 ponto |
| 2 | 14:41 | 129,44 | 428,97 | 2,34 | 0,301753 | variação natural ~5 V em 23 min |
| 3 | 14:43 | 129,70 | 433,39 | 2,71 | 0,299269 | variação natural |
| 4 | 14:51 | 129,96 | 435,51 | 2,66 | 0,298417 | 1 leitura (1360,2) descartada — erro de digitação |
| 5 | 14:54 | 129,35 | 436,42 | 2,21 | 0,296389 | ⚠️ pareamento **frágil** (leituras antes da janela) |

Todas: multímetro ANENG M118A, `±0,5%+3d`, terminais do ZMPT.

**Por que 5 pontos "de graça"?** A rede se mexeu sozinha: entre 14:18 e
14:54 a tensão naturalmente variou de 123,9 → 130,0 V. Sem variac, essa
**variação natural** substituiu o autotransformador. É o "fallback de
pontos naturais" previsto no plano.

---

## 4. Regressão (intercepto forçado a 0)

`K = Σ(a·v) / Σ(a²)` — mínimos quadrados com intercepto fixo em zero
(faz sentido fisicamente: sem sinal, `vrms` deve ser 0).

```
=== REGRESSAO (n=5) ===
K FINAL = 0.298684   (0,2987 V RMS/conta p-p)
K_i:     [0.29762, 0.30175, 0.29927, 0.29841, 0.29639]
Incerteza de K = ± 0.001799  (0,60%)
max|K_i − K|/K = 0,82%

  amp      V_ref   previsto   erro%
  416.3  123.90    124.34   +0.36%
  429.0  129.44    128.13   −1.01%
  433.4  129.70    129.45   −0.20%
  435.5  129.96    130.08   +0.09%
  436.4  129.35    130.35   +0.77%

Faixa: 123.9–130.0 V (span 4,7%)  |  maior erro 1,01% → PASSA ≤ ±5%
```

**Evolução do `K` conforme os pontos entraram:**

| Pontos | K | Incerteza |
|---|---|---|
| 1 | 0,297651 | não quantificável |
| 2 | 0,299746 | ±0,69% |
| 3 | 0,299581 | ±0,57% |
| 4 | 0,299279 | ±0,52% |
| 5 | **0,298684** | **±0,60%** |

Repare: do ponto 3 ao 5 o `K` mudou <0,4% — **estabilizou**. O ponto 5
(frágil) mexeu só −0,20%.

---

## 5. Status honesto — o que vale e o que não vale

### ✅ O que foi atingido
- **Linearidade verificada** — 5 pontos caem numa reta com erro ≤ 1,01%.
- **Incerteza quantificada** (±0,60%) — saiu de "não quantificável".
- **Nenhum ponto saturado.**
- **Faixa do 1º ponto reproduzível** — o `K` é estável (<0,4% entre 3 e 5 pontos).

### ❌ O que NÃO foi atingido
- **Não cobre PRODIST** (90–150 V, span ~45%). Testamos **span de 4,7%**.
- Logo: **serve para o monitor** (volts, swell/under‑voltage, mostrar
  "127 V" no Discord/MQTT) — mas **NÃO vale para apuração DRP/DRC do
  MedTens**, que exige os 9 pontos da ADR‑0006.

> **Mudou de natureza, não de categoria:** deixou de ser *"parcial por
> falta de ponto"* e virou *"linear real verificada numa faixa estreita"*.
> O gargalo agora é a **faixa**, não a **quantidade** de pontos.

---

## 6. Decisão: `K` no firmware = 0,2976 (mantido)

O firmware foi gravado (18/10, 14:32) com `K_VRMS = 0.2976` — o valor do
**ponto 1**, único disponível na hora. A regressão final dá **0,2987**.

**Decisão (18/10): manter `0,2976` e corrigir na próxima gravação.**

| | |
|---|---|
| Delta | 0,2976 → 0,2987 = **0,37%** |
| Comparação | **menor que a incerteza do próprio multímetro** (~0,5–0,7%) |
| Custo de regravar agora | 3ª gravação USB + **validação da v3.3 recia do zero** |
| Plano | aplicar `0,2987` (ou melhor) **na próxima gravação** — ex.: junto do OTA |

**Não regravar por 0,37%** é decisão correta: o ganho real seria menor
que o ruído da medição.

---

## 7. Próximo passo — plano de medições SEGUNDA 19/10/2026

> 📌 **ANOTADO (18/10):** amanhã, **segunda-feira 19/10**, faremos
> **3 janelas de medição: manhã, tarde e noite.**

**Objetivo: abrir a faixa.** O gargalo é o span (4,7%). 3 momentos do dia
em 3 tensões diferentes (a rede varia com a carga da vizinhança) devem
ampliar o span **sem nenhum hardware perigoso**.

| Janela | Por quê | Esperado |
|---|---|---|
| **Manhã** | pico de uso industrial/comercial começa; amanhecer ainda leve | tensão mais **alta** (menos carga residencial) |
| **Tarde** | pico solar + carga mista | tensão **intermediária** |
| **Noite** | pico residencial (chuveiro, forno, ar) na mesma fase | tensão mais **baixa** — **é a janela mais valiosa** |

**Para cada janela:**
1. Rodar `tools/calibracao_captura.py` (60 s de `amp`).
2. Ler `V_ref` no multímetro **no mesmo instante** (mesmos terminais do ZMPT).
3. Gravar no CSV (mesmo formato — o script já anota tudo).
4. ⚠️ **Pareamento importa** — o ponto 5 foi "frágil" justamente porque as
   leituras chegaram antes da janela. Ler **durante** a captura.

**Além disso (se der):** 1 leitura com **carga pesada ligada** (chuveiro +
forno na mesma fase) — é o jeito mais fácil de forçar uma tensão baixa
controlada, e não depende de hora. Pode levar o span de 4,7% para ~12%.

**Meta:** span ≥ ~10% para a incerteza do `K` cair de forma real. Com
faixa ampla, aí sim o `vrms` se aproxima do critério PRODIST (ainda que a
varredura completa de 9 pontos continue dependendo do **variac**).

---

## 8. Sessão noturna de 18/10 — a variação natural apareceu (sem pareamento)

**Quando:** 18/10, 19:50–23:00. **Resultado: nenhuma linha nova no CSV**, por
motivo de método — mas a sessão valeu por dois achados.

### 8.1 A variação natural que o plano pedia existiu sozinha

O §7 pedia um variac porque "sem variac não há span". Na noite de 18/10 a
própria rede entregou a variação, sem ninguém tocar em nada:

| Hora | Multímetro | `amp` do aparelho |
|---|---|---|
| 19:5x | 121,7 | 418 |
| 20:0x | 121,8 – 122,2 | 420 |
| 20:01 | 124,4 | 423 |
| 20:02 | 124,5 | 421,5 (média, n=90) |

**121,7 → 124,5 V em ~5 minutos** — 2,3% de variação só de olhar. É o
padrão de carga da noite caindo. Confirma a hipótese do §7 de que o span
não precisa de variac: precisa de **janelas em horários diferentes**.

Também se observou uma queda até **119,8 V** às 19:54 (−5,7% do nominal),
antes da subida. Se repetir, sozinha isso levaria o span de 4,7% para ~9%.

### 8.2 O `k` não bateu — mas não dá para afirmar que o K está errado

Com `amp` médio 421,5 e o multímetro em 124,5 V:

```
k = 124,5 / 421,5 = 0,295358        K aplicado no firmware: 0,2987
```

Ou seja, o aparelho estaria **+1,15% alto** — fora da incerteza de ±0,60%
da regressão do §4. Com a leitura de 121,8 V daria 0,289000, **2,6% abaixo**.

**Não está gravado no CSV, e não deve ser.** As leituras do multímetro
chegaram **fora da janela de captura** — o mesmo defeito do ponto 5, só
que pior, porque a tensão estava subindo 0,5 V/min e casar errado produz
exatamente essa divergência. É impossível separar "K errado" de
"pareamento errado" com estes dados. **A hipótese de K errado fica ABERTA.**

### 8.3 O que mudou para a segunda (19/10)

Nasceu um protocolo de sincronia, porque a latência da conversa não é
menor que a deriva da tensão:

1. `python3 testes/amostrar.py 20` — janela com `amp_media`, `sigma`, `min`,
   `max`, `n` (o CSV exige estatística, leitura isolada não serve)
2. **Ler o multímetro durante a janela** e mandar o valor
3. Parear pelo carimbo de tempo

Arquivos de apoio, em `data/testes-campo-2026-10-18/`:

| Arquivo | Conteúdo |
|---|---|
| `amostras.tsv` | 1 MB, ~3 amostras/s do `/estado`, com timestamps |
| `serial.log` | log serial do teste de queda (gracia medida) |
| `powercycle.log` | transições do teste de corte de energia |
| `estado.log` | transições de `/estado` do teste do ZMPT |

> ⚠️ **Não abrir a porta serial para observar.** No CH340 o RTS é o pino EN
> do ESP32; a linha de auto-reset que serve para flashar **reinicia o
> aparelho**. Custou um reboot em produção às 19:26:39. Observar só por
> HTTP (`/estado`), que responde mesmo com a placa viva.

---

## 9. Arquivos

| Arquivo | Conteúdo |
|---|---|
| `data/calibracao_2026-10-18.csv` | os 5 pontos, formato do plano |
| `tools/calibracao_captura.py` | captura `amp` (60 s) + estatísticas + saturação |
| `analysis/calibracao_regressao.py` | regressão intercepto 0, `K` ± incerteza, erros |
| `esp32/include/config.h` | `K_VRMS 0.2976`, `V_NOM 127` (não versionado) |
| `esp32/include/config.example.h` | mesmos parâmetros **versionados**, com nota |
| [`plano-ota-qualidade.md`](plano-ota-qualidade.md) | Prioridade 2 (metodologia de 9 pontos) |
| [`esp32-update-antes-depois.md`](esp32-update-antes-depois.md) | status do upgrade v3.3 |
