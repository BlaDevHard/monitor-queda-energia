# Protocolo UART — Sensor ↔ Gateway

> **v3.1** — sensor com ZMPT101B, gateway ESP32.
> A v2 (ESP-01, publicada) está documentada na seção
> [*Versão 2*](#versão-2--zmpt--esp-01-publicada) abaixo.

## Versão 3.1 — detecção direta da rua (ZMPT)

O Arduino mede a **própria rede**, pelo ZMPT101B no A0: amplitude
pico-a-pico do sinal alternada numa janela de ~165 ms (~10 ciclos de
60 Hz). Sem tensão calculada, sem calibração de voltagem.

> **Por que não mais "pela fonte"?** A v3.0 tentava detectar a queda
> pela troca `tomada → power bank` na alimentação do próprio chip
> (bandgap interno). Isso exigia calibrar um limiar de Vcc, dependia
> do power bank assumir limpo (risco de pass-through) e **não media a
> rua**. Com o ZMPT medindo a rede direto, aquilo era redundante. Os
> campos `fonte` e `vcc` saíram junto.

### Camada física

- UART **9600 8N1**, sem controle de fluxo
- TX do ATmega (5 V) → **divisor resistivo 1 kΩ + 2 kΩ** → RX do ESP32 **GPIO16** (3,3 V)
- Linha ociosa em nível alto; uma mensagem por linha terminada em `\n`
- Direção única: o gateway só **escuta** (não envia comandos ao sensor)
- **Nenhuma linha é enviada antes da 1ª confirmação** (evita estado falso no boot)

### 1. Boot do sensor

```json
{"boot":"Sensor OK","versao":"3.1"}
```

O gateway **ignora** esta linha (não possui o campo `energia`).

### 2. Estado + medição

Enviada continuamente (~a cada 500 ms) depois da primeira confirmação:

```json
{"energia":"presente","amp":417}
{"energia":"ausente","amp":7}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `energia` | string | `presente` / `ausente` — **único campo que move a máquina de estados** (contrato mantido desde a v2) |
| `amp` | inteiro | amplitude pico-a-pico do A0 na última janela (~165 ms), em contagens 0–1023 |

> `amp` **não é tensão**. É contagem do ADC. O sistema não calcula
> V_RMS em nenhum ponto.

### 3. Linhas de diagnóstico (ignoradas pelo gateway)

```
#ZMPT amp=417 [min=302 max=719] f=59.5Hz  bruto=ENERGIA  conf=3
```

Não começam com `{` e caem fora do filtro do gateway. Levam também a
**frequência medida** (`f=`), que só aparece quando há sinal — é o que
separa "rua presente" de "ruído abaixo do limiar".

### Semântica

| Regra | Valor | Onde |
|---|---|---|
| Limiar de presença | `amp >= 65` → `presente` | Arduino (`LIMITE_AMPLITUDE`) |
| Debounce | 3 leituras idênticas seguidas (~1,5 s) | Arduino (`CONFIRMACOES`) |
| Antes da 1ª confirmação | nenhuma linha é enviada | Arduino |
| Transição | publica `QUEDA` / `SERVIÇO ATIVO` | Gateway |
| 1ª linha após o boot | publica o estado atual | Gateway |
| Sem campo `energia` | ignorada | Gateway |
| Silêncio > 45 s | alerta `⚠️ SENSOR EM SILÊNCIO` | Gateway |

Valores medidos nesta instalação (out/2026):

| Cenário | `amp` | `f` |
|---|---|---|
| Rua com energia | ~417–424 | 59,5 Hz |
| Rua sem energia (ZMPT alimentado) | ~7 | — (ruído) |
| Ruído típico do ADC | < 10 | — |

### Exemplo de sessão

```
→ {"energia":"presente","amp":417}
→ {"energia":"presente","amp":416}
   ... rua cai ...
→ {"energia":"presente","amp":418}     última leitura antes da queda
→ {"energia":"ausente","amp":9}         ↓
→ {"energia":"ausente","amp":7}          debounce (3 idênticas)
→ {"energia":"ausente","amp":8}         confirma!
   [gateway carimba a hora no RTC interno]
   [sem internet → mensagem entra na FILA]
   ... roteador volta ...
   [gateway entrega a fila: 1 msg/s]
   [Discord: QUEDA — vermelho, com a hora do RTC]
   ... rua volta ...
→ {"energia":"presente","amp":415}
→ {"energia":"presente","amp":417}
→ {"energia":"presente","amp":416}      confirma!
   [Discord: SERVIÇO ATIVO — verde + início + retorno + duração]
```

---

## Versão 2 — ZMPT + ESP-01 (publicada)

### Camada física

- UART **9600 8N1**, sem controle de fluxo
- TX do ATmega (5 V) → **divisor resistivo 1 kΩ + 2 kΩ** → RX do ESP-01 (3,3 V)
- Linha ociosa em nível alto; uma mensagem por linha terminada em `\n`
- Direção única: o gateway só **escuta** (não envia comandos ao sensor)

## Mensagens (JSON por linha)

### 1. Boot do sensor

Enviado uma única vez ao ligar:

```json
{"boot":"Sensor OK","versao":"2.1"}
```

O gateway **ignora** esta linha (não possui o campo `energia`).

### 2. Estado da rede

Enviada continuamente (~a cada 665 ms) depois da primeira confirmação:

```json
{"energia":"presente","amp":94}
{"energia":"ausente","amp":8}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `energia` | string | `presente` ou `ausente` — **único campo usado pelo gateway** |
| `amp` | inteiro | amplitude pico-a-pico do ADC na última janela (~165 ms) — diagnóstico |

> **Nota:** `amp` é medida em contagens do ADC (0–1023), **não** é tensão.
> O sistema não calcula V_RMS em nenhum ponto.

## Semântica

| Regra | Valor | Onde |
|---|---|---|
| Limiar de presença | `amp >= 65` → `presente` | Arduino (`LIMITE_AMPLITUDE`) |
| Debounce | 3 leituras idênticas seguidas (~2 s) | Arduino (`CONFIRMACOES`) |
| Antes da 1ª confirmação | nenhuma linha é enviada | Arduino |
| Transição de estado | publica `QUEDA` / `SERVIÇO ATIVO` | Gateway |
| 1ª linha após o boot | publica o estado atual | Gateway |
| Linhas sem campo `energia` | ignoradas | Gateway |

Valores de referência medidos em campo:

| Cenário | `amp` |
|---|---|
| Rua com energia (127 V) | ~94 |
| Rua sem energia (ZMPT alimentado) | ~40 (instável → por isso o limiar é 65) |
| Ruído típico do ADC | < 10 |

## Pseudocódigo do sensor

```
a cada ciclo (~665 ms):
    janela = 1500 leituras do A0 (~165 ms)
    amp = max(janela) - min(janela)
    bruta = (amp >= 65)

    se bruta == ultima_bruta:
        contador += 1     (limitado a 3)
    senão:
        ultima_bruta = bruta
        contador = 1

    se contador >= 3:
        confirmado = bruta
        ja_confirmou = true

    se ja_confirmou:
        enviar JSON { energia: confirmado ? "presente" : "ausente", amp }
```

## Exemplo de sessão

```
→ {"energia":"presente","amp":96}     (rede normal, sem eventos)
→ {"energia":"presente","amp":93}
   ... rua cai ...
→ {"energia":"presente","amp":91}     última leitura antes da queda
→ {"energia":"ausente","amp":42}      ↓
→ {"energia":"ausente","amp":38}       debounce (3 idênticas)
→ {"energia":"ausente","amp":44}      confirma!
   [gateway publica: QUEDA — vermelho]
   ... durante a queda: linhas "ausente" seguem, nenhum evento novo ...
   ... rua volta ...
→ {"energia":"presente","amp":95}
→ {"energia":"presente","amp":97}
→ {"energia":"presente","amp":94}      confirma!
   [gateway publica: SERVIÇO ATIVO — verde + duração da queda]
```
