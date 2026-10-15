# Protocolo UART — Sensor ↔ Gateway

> **v3.2** — sensor com ZMPT101B, gateway ESP32.
> A v2 (ESP-01, publicada) está documentada na seção
> [*Versão 2*](#versão-2--zmpt--esp-01-publicada) abaixo.

## Versão 3.2 — detecção direta da rua + pulso curto (ZMPT)

O Arduino mede a **própria rede**, pelo ZMPT101B no A0: amplitude
pico-a-pico do sinal alternada numa janela de ~55 ms (~3,3 ciclos de
60 Hz). Sem tensão calculada, sem calibração de voltagem.

> **O que mudou da v3.1 para a v3.2.** A v3.1 só via queda longa: janela
> de 165 ms + exigir 3 leituras idênticas em ciclos de ~665 ms davam
> ~2 s de debounce no Arduino. Um piscar de lâmpada (250–500 ms) morria
> antes de ser reportado, e o gateway nunca ficava sabendo que existiu.
> Um blecaute de 5 s com 3 quedas e 3 retornos virava **um** evento.
>
> Na v3.2 o Arduino **reporta todo pulso**, com a duração que ele mesmo
> mede. O debounce do Discord/Pushbullet mudou para o gateway, que tem
> relógio, memória e contexto para juntar vários pulsos **num relatório
> só**.

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
{"boot":"Sensor OK","versao":"3.2"}
```

O gateway **ignora** esta linha (não possui o campo `energia`).

### 2. Estado + medição

Enviada quando o estado **muda**, e no heartbeat de 5 s para o gateway
não achar que o Arduino morreu:

```json
{"energia":"presente","amp":417}
{"energia":"ausente","amp":7}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `energia` | string | `presente` / `ausente` — **único campo que move a máquina de estados** (contrato mantido desde a v2) |
| `amp` | inteiro | amplitude pico-a-pico do A0 na última janela (~55 ms), em contagens 0–1023 |

> `amp` **não é tensão**. É contagem do ADC. O sistema não calcula
> V_RMS em nenhum ponto.

### 3. Pulo — a novidade da v3.2

Cada queda e cada volta vira **um evento próprio**, com a duração medida
pelo próprio Arduino. É o que permite ver um piscar de lâmpada que o
estado (`presente`/`ausente`) sozinho não mostraria.

```json
{"pulso":1,"dur_ms":420,"amp_min":428}
{"pulso":1,"abrindo":1,"amp":7}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `dur_ms` | inteiro | duração **medida** do pulso, do primeiro `abrindo` até o retorno (ms) |
| `amp_min` | inteiro | amplitude na janela do retorno — `amp` normalmente alto (~420) |
| `abrindo` | 1 | pulso **em curso**: o Arduino repete a cada ~200 ms enquanto dura. Só existe quando o pulso passa de ~200 ms |
| `amp` | inteiro | amplitude na janela em que a linha foi gerada (≈7 enquanto caiu) |

> Uma linha `dur_ms` **sem** `abrindo` anterior é normal: um pulso de
> 150 ms cabe dentro de uma janela e termina antes do próximo heartbeat.
> O gateway aceita os dois formatos.

**No gateway:** as linhas `pulso` são guardadas num buffer (até 64) e o
relatório só sai quando passam **5 s sem pulso nenhum** — é a janela de
agrupamento. Enquanto o buffer tem pulsos, as transições de estado
**não** publicam QUEDA/SERVIÇO ATIVO no Discord (elas ficam no MQTT e no
log serial). O relatório carrega horário e duração de cada um, então
nada se perde.

### 4. Linhas de diagnóstico (ignoradas pelo gateway)

```
#ZMPT amp=417 [min=302 max=719] f=60.7Hz  bruto=ENERGIA  conf=1
```

Não começam com `{` e caem fora do filtro do gateway. Levam também a
**frequência medida** (`f=`), que só aparece quando há sinal — é o que
separa "rua presente" de "ruído abaixo do limiar".

### Semântica

| Regra | Valor | Onde |
|---|---|---|
| Limiar de presença | `amp >= 65` → `presente` | Arduino (`LIMITE_AMPLITUDE`) |
| Amostragem | 500 amostras (~55 ms) por janela | Arduino (`AMOSTRAS`) |
| Ritmo | ciclo-alvo 60 ms, auto-ajustado (fecha com o que falta) | Arduino (`CICLO_MS`) |
| Debounce do **pulso** | nenhum — reporta tudo | Arduino (`CONFIRMACOES 1`) |
| Debounce do **estado** | 3 mudanças + janela de agrupamento de 5 s | Gateway |
| Antes da 1ª confirmação | nenhuma linha de estado é enviada | Arduino |
| Transição isolada | publica `QUEDA` / `SERVIÇO ATIVO` | Gateway |
| Transição durante pulsos | **silenciada** — entra no relatório | Gateway |
| Vários pulsos em 5 s | **1 relatório** com todos | Gateway |
| 1ª linha após o boot | publica o estado atual | Gateway |
| Sem campo `energia` e sem campo `pulso` | ignorada | Gateway |
| Silêncio > 45 s | alerta `⚠️ SENSOR EM SILÊNCIO` | Gateway |

Valores medidos nesta instalação (out/2026):

| Cenário | `amp` | `f` |
|---|---|---|
| Rua com energia | ~417–424 | 59,5 Hz |
| Rua sem energia (ZMPT alimentado) | ~7 | — (ruído) |
| Ruído típico do ADC | < 10 | — |

### Exemplo de sessão

**Blecaute longo** (queda de verdade):

```
→ {"energia":"presente","amp":417}
   ... rua cai ...
→ {"pulso":1,"abrindo":1,"amp":7}
→ {"energia":"ausente","amp":7}
   [gateway: EVENTO: QUEDA — vai no relatorio, nao manda avulso]
   ... 4 s depois a rua volta ...
→ {"pulso":1,"dur_ms":4020,"amp_min":428}
→ {"energia":"presente","amp":428}
   [gateway: EVENTO: SERVICO ATIVO — idem]
   ... 5 s sem pulso → janela fecha ...
   [gateway: RELATORIO DE PULSOS — 1 evento, 4020 ms]
   [Discord: 1 mensagem so, laranja]
```

**Rajada de piscadas** (o caso da v3.1 perdido):

```
   ... 3 viradas de disjuntor em 5 s ...
→ {"pulso":1,"dur_ms":246,"amp_min":682}
→ {"pulso":1,"dur_ms":247,"amp_min":472}
→ {"pulso":1,"dur_ms":307,"amp_min":435}
   ... 5 s sem pulso → janela fecha ...
   [gateway: RELATORIO DE PULSOS — 3 eventos, 0,8 s no total]
   [Discord: 1 mensagem com os 3 horarios e duracoes]
   [MQTT: estado atualizado em cada virada, como sempre]
```

Testado em campo em 15/10/2026: 10 viradas de disjuntor em ~20 s
produziram **2 mensagens** no Discord (2 rajadas separadas por mais de
5 s), com pulsos de 246–426 ms detectados. A v3.1 exigiria ~2 s por
evento e teria visto no máximo um deles.

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
