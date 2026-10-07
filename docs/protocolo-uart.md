# Protocolo UART — Sensor ↔ Gateway

## Camada física

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
