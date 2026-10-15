# Arquitetura

> **Versão 3.1** (em desenvolvimento). A v2 — ZMPT101B + ESP-01, publicada e em
> campo — continua descrita em [`../README.md`](../README.md) e em
> [protocolo-uart.md](protocolo-uart.md).

## Visão geral

Pipeline de 2 estágios conectados por UART:

```
┌────────────────────────────┐   UART 9600   ┌────────────────────────────┐  HTTPS
│         ATmega328P         │ ────────────▶ │      ESP32 (WiFi + RTC)    │ ──────▶ Discord
│   mede a RUA pelo ZMPT     │  linhas JSON  │  carimba hora, monta fila  │        Pushbullet
│        no pino A0          │               │  publica quando a rede volta│       MQTT + HTTP
└────────────────────────────┘               └────────────────────────────┘
              (sensor)                                  (gateway)
```

**O que mudou da v2 para a v3.1:**

| | v2 (em campo) | v3.1 |
|---|---|---|
| Sensor | ZMPT101B em A0 | **ZMPT101B em A0** (mantido) |
| Gateway | ESP-01 | **ESP32 com RTC** (interno, sem módulo) |
| Hora | só NTP | **RTC interno → NTP → checkpoint na flash** |
| Alimentação | no-break | **power bank** |
| Rede fora | mensagem perdida | **fila de mensagens** na RAM |
| Power bank acabou | — | **checkpoint na flash** → `QUEDA (recuperada)` |

### A v3.0 tentou e voltou atrás

A v3.0 retirou o ZMPT e passou a detectar a queda pela **própria
alimentação do Arduino**: com rua, o chip na tomada; sem rua, o power
bank assume a USB e a Vcc cai. Medida pelo **bandgap interno de 1,1 V**.

Não sobreviveu ao teste de campo:

- exigia **calibrar um limiar de Vcc** entre dois níveis que ninguém tinha
  como medir sem multímetro;
- dependia do **power bank assumir limpo** na troca (pass-through não é
  garantido e alguns bancos reiniciam);
- **não media a rua** — era um palpite sobre a rede, indireto.

O ZMPT voltou ao A0 e mede a rede direto. `VCC_LIMITE_MV`,
`VCC_HISTERESE_MV`, `BATERIA_MENOR`, `fonte` e `vcc` foram removidos.

## Estágios

### 1. Sensor — ATmega328P (Arduino Uno)

- **Hardware**: Arduino Uno + **ZMPT101B no A0**.
- **Função**: detectar se a **rua tem energia**, e o instante em que cai
  e volta.
- **Como mede**: amplitude pico-a-pico do sinal alternada numa janela de
  500 amostras (~55 ms — cabe em **1 ciclo** de 60 Hz, o que permite ver um
  piscar de lâmpada).

  ```
  amp = max(janela) − min(janela)      presente se amp >= 65
  ```

  Medido nesta instalação: rua com energia **~417**, sem energia **~7**.
  A frequência medida (`f=60.7Hz`) confirma que é rede de verdade e não
  ruído — ela só é reportada quando há sinal, e é a **média de 20 janelas**
  (~1 s), porque uma janela de 55 ms sozinha faz o valor oscilar entre 53
  e 71 Hz.
- **Sem debounce no Arduino** (`CONFIRMACOES 1`): ele reporta **todo pulso**
  com a duração medida. Quem separa blecaute de piscada é o ESP32, que tem
  a duração em mãos (ver [protocolo-uart.md](protocolo-uart.md)).
- **Saída**: UART 9600 8N1, uma linha JSON por ciclo de ~60 ms
  (ver [protocolo-uart.md](protocolo-uart.md))
- **LED**: aceso = QUEDA (sem serviço)
- **Não calcula tensão**: o projeto nunca precisou de V_RMS, só de
  presença.

### 2. Gateway — ESP32

- **Função**: ler a UART, **carimbar a hora com o RTC**, manter o estado e
  publicar eventos — mesmo sem internet no momento do evento.
- **Conectividade**: estação (STA) na rede 2,4 GHz.
- **RTC sem módulo externo**: o `system time` do ESP32 continua correndo
  enquanto a placa estiver energizada, calibrado contra o cristal de 40 MHz.
  Três camadas, na ordem:
  1. **NTP** — autoritativo; só sincroniza quando há rede
  2. **RTC interno** — mantém a hora durante a queda (sem NTP)
  3. **Checkpoint na flash** — epoch salvo, usado quando a placa perde energia
- **Sem NTP, a hora é marcada como aproximada** na própria mensagem.
- **Destinos**: Discord (webhook), Pushbullet (token), MQTT retido, HTTP local.
- **Segredo**: WiFi/tokens/identificação ficam em `config.h` (**não versionado**).

## Máquina de estados

```
                  3 leituras "ausente" seguidas (~1,5 s)
      PRESENTE ─────────────────────────────────────────▶ AUSENTE
         ▲                                                   │
         │       3 leituras "presente" seguidas (~1,5 s)     │
         └───────────────────────────────────────────────────┘
                    publica: QUEDA (vermelho)     publica: SERVIÇO ATIVO
                                                     (verde) + duração
```

- **1ª leitura após o boot** publica o estado atual, junto com o `SISTEMA ONLINE`
- Sem mensagens repetidas enquanto o estado não muda
- Se o checkpoint dizia "em queda" e a 1ª leitura já mostra serviço →
  `QUEDA (recuperada)`: a placa desligou no meio do episódio

## Fluxo de dados

1. Arduino mede uma janela de 500 amostras do A0 (~55 ms)
2. `amp = max − min`; `presente` se `amp >= 65`
3. Sem debounce: reporta a transição (e todo pulso medido) direto
4. Envia linha JSON por UART (ciclo alvo de ~60 ms)
5. Gateway lê a linha e aplica a máquina de estados
6. **Pulsos vão para um buffer**; quando o sinal volta e passam 5 s em
   silêncio, a fila inteira vira **um relatório só**
7. Queda: o ESP32 **adiada a decisão 8 s** — se a rua voltar antes, vira
   relatório; se não, é blecaute e publica **QUEDA**
8. Se há internet → envia. Se **não há** → guarda na **fila**
9. Quando o WiFi volta, entrega **1 mensagem por segundo** (rate limit do webhook)

## Alimentação

```
tomada ─ adaptador ─▶ USB do power bank (pass-through)
                              ├── USB 1 ─▶ Arduino
                              └── USB 2 ─▶ ESP32
rua ─▶ ZMPT101B ─▶ A0 do Arduino   (o sinal da rede, independente
                                    de quem esteja alimentando)
```

- A **queda de rua é detectada pelo ZMPT**, que mede a rede direto. A
  alimentação não entra na equação de detecção — só precisa durar.
- **Roteador cai junto com a rua** → sem internet durante a queda → daí a
  **fila de mensagens** e o **RTC** serem obrigatórios, não opcionais.
- Se o **power bank esgotar** e tudo desligar, o **checkpoint na flash**
  permite reconstruir o episódio no boot.

### Autonomia estimada — power bank de 10 000 mAh

Energia estocada: 10 000 mAh × 3,7 V = **37 Wh**. Com o boost a ~85%,
ficam **~6 300 mAh disponíveis a 5 V**.

| Carga (a 5 V) | Corrente |
|---|---|
| Arduino Uno (ATmega328P + 16U2 + regulador + LED) | ~55 mA |
| ESP32 DevKit (WiFi ligado, HTTP/MQTT ocasionais) | ~120 mA |
| **Total típico** | **~175 mA** |

→ **~36 h** em operação normal.

No caso que importa — **a rua caiu, o roteador morre junto** — o ESP32
varre canais procurando rede, a ~250 mA:

→ **~20 h** de autonomia real numa queda.

Ressalvas: um banco "10 000 mAh" barato costuma entregar 5 000–6 500 mAh
na USB; bancos ruins desligam abaixo de ~50 mA de carga (a nossa, ~175 mA,
está seguro); e o **pass-through** não é garantido.

### ⚠️ Verificar antes de montar

O **power bank mantém a saída USB ligada enquanto carrega?** Muitos desligam
a saída durante o carregamento — nesse caso o ESP32 ficaria desligado sempre
que houvesse tomada. Teste de 30 s: power bank na tomada, celular nele.

Detalhes e calibração: [`../esp32/README.md`](../esp32/README.md).
