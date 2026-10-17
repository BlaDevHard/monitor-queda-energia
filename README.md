# ⚡ Monitor de Quedas de Energia — Antonina-PR

> ### 📌 Status: duas versões neste repositório
>
> | | |
> |---|---|
> | **v2 — em campo e publicada** | ZMPT101B → Arduino → ESP-01. É o que este README descreve abaixo. |
> | **v3.1** | ZMPT **direto no A0 do Arduino** (o único detector da rua), ESP-01 **substituído por ESP32 com RTC + NTP**, e **power bank** no lugar do no-break. |
> | **v3.2 — a mais recente** | **Detecta pulso curto**: piscar de lâmpada (250 ms+) vira evento, e várias quedas num intervalo de 5 s saem **empacotadas numa mensagem só**. Documentação: [`esp32/README.md`](esp32/README.md) · protocolo: [`docs/protocolo-uart.md`](docs/protocolo-uart.md). |
>
> A v3.2 foi validada **em campo**, derrubando o disjuntor de verdade:
> 10 viradas em ~20 s → 2 mensagens no Discord (pulsos de 246–426 ms).
> Ainda **não caiu a rua de verdade** durante um registro — a primeira queda
> real é o teste que falta. Enquanto isso, as seções abaixo valem para a v2.

Sistema embarcado, de baixo custo e **sem assinaturas**, que detecta queda e
restauração da energia da rede (**COPEL**) em Antonina-PR e publica cada evento em:

- 💬 **Discord** — log auditável com data/hora (webhook do canal)
- 📲 **Pushbullet** — notificação instantânea no celular

Sem IFTTT e sem assinatura: Discord webhook + Pushbullet + NTP.

## Visão geral

| | |
|---|---|
| **O que detecta** | Presença de energia na rua — **não calcula tensão** |
| **Como** | ZMPT101B → Arduino Uno → UART → ESP-01 → WiFi → Discord/Pushbullet |
| **Latência** | ~3–5 s do evento até a mensagem chegar |
| **Timestamp** | NTP (GMT-3) no corpo + data/hora do próprio Discord |
| **Alimentação** | Arduino + ESP-01 + roteador no **no-break** |
| **Custo recorrente** | **R$ 0** |

## Como funciona

O Arduino **não calcula tensão**. Ele apenas verifica, a cada ~665 ms, se o
ZMPT101B está recebendo alternância da rua (amplitude pico-a-pico do ADC):

| Situação | JSON enviado | Estado |
|---|---|---|
| Rua com energia | `{"energia":"presente","amp":94}` | `SERVIÇO ATIVO` |
| Rua sem energia | `{"energia":"ausente","amp":8}` | `QUEDA` |

Regras do sistema:

1. **Debounce de ~2 s** — o estado só muda após **3 leituras idênticas
   seguidas**; interferência isolada não gera alarme falso
2. **Alternância** — `QUEDA` (vermelho) ↔ `SERVIÇO ATIVO` (verde); durante a
   queda **não há mensagens repetidas**
3. **Boot publica o estado** — ao ligar, além do `SISTEMA ONLINE`, o sistema
   envia o estado atual. Se a bateria do no-break acabar, quando tudo voltar o
   log continua coerente
4. **Duração** — quando a energia volta, a mensagem inclui quanto tempo durou a queda

## Mensagens

Exemplo real enviado ao Discord/Pushbullet:

```
SERVIÇO ATIVO
Energia restabelecida
Duração da queda: 1m 59s
Local: SEU_MUNICIPIO
CEP: 00000000
UNIDADE CONSUMIDORA: 000000000000000
Hora: 07/10/2026 13:29:23
```

| Evento | Título | Cor |
|---|---|---|
| Sistema liga | `SISTEMA ONLINE` | ciano |
| 1ª leitura após boot | `QUEDA` ou `SERVIÇO ATIVO` | vermelho / verde |
| Energia caiu | `QUEDA` | vermelho |
| Energia voltou | `SERVIÇO ATIVO` (com duração) | verde |

> Os dados de identificação (município, CEP e unidade consumidora) ficam no
> `config.h` de cada usuário — **não são versionados**.

## Arquitetura

```
 REDE ELÉTRICA (COPEL, 127 V / 60 Hz)
              │
        ┌─────┴─────┐
        │  ZMPT101B  │  sensor de isolamento
        └─────┬─────┘
              │ sinal ≈ 0,16 Vrms
         A0 ──┤
      ┌───────┴────────────┐                        ┌──────────────┐
      │   Arduino Uno      │  UART 9600 (divisor    │    ESP-01    │
      │  detecta presença  │──1 kΩ + 2 kΩ, 5V→3V3)─▶│   gateway    │
      │  (ZMPT → A0)       │◀────── RX ◀────────────│  (WiFi STA)  │
      └───────┬────────────┘                        └──────┬───────┘
              │ 5V                                         │
        ┌─────┴──────┐                               WiFi 2,4 GHz
        │ AMS1117 3V3│──▶ VCC + CH_PD do ESP-01          │
        └────────────┘                     ┌─────────────┼─────────────┐
                                            ▼             ▼             ▼
                                       Discord       Pushbullet       NTP
                                     (webhook)      (celular)     (hora GMT-3)
```

- **Por que dois chips?** O ATmega não tem WiFi; o ESP-01 só faz a ponte de rede
- **Por que 3,3 V?** O ESP-01 **nunca** pode receber 5 V — o divisor 1 kΩ/2 kΩ
  protege o pino RX na gravação; na operação o ESP é alimentado pelo Arduino
  (5 V → AMS1117-3.3 V), pois o pino 3.3 V do Uno entregaria só ~50 mA
  (o WiFi pica em 300 mA)

## Hardware

| Peça | Quantidade | Função |
|---|---|---|
| Arduino Uno | 1 | detecta a presença da energia |
| ZMPT101B | 1 | lê a rede elétrica com isolamento |
| ESP-01 (1 MB) | 1 | gateway WiFi |
| FTDI FT232 (USB-Serial) | 1 | apenas para **gravar** o ESP-01 |
| Resistores 1 kΩ e 2 kΩ | 1 cada | divisor 5 V → 3,3 V |
| Regulador AMS1117-3.3 V | 1 | alimenta o ESP-01 pelo Arduino |
| No-break | 1 | mantém Arduino + roteador vivos na queda |

### Ligações

```
ZMPT101B        Arduino Uno
  OUT ─────────▶ A0
  VCC ────────── 5V
  GND ────────── GND

Arduino Uno      ESP-01
  pino 1 (TX) ─ [1 kΩ] ─┬─▶ RX (GPIO3)      (só na gravação via FTDI)
                        └─ [2 kΩ] ─ GND
  5V ─ [AMS1117-3.3] ─▶ VCC  e  CH_PD (EN)
  GND ────────────────▶ GND

FTDI (gravação)       ESP-01
  TXD ───────────────▶ RX (GPIO3)   ← sempre cruzado
  RXD ◀─────────────── TX (GPIO1)
  GND ───────────────▶ GND
```

⚠️ **TX → RX e RX → TX sempre cruzados.** Na operação, desconecte os fios de
sinal do FTDI (mantenha só alimentação, se quiser) — senão ele briga com o
Arduino na mesma linha.

## Firmware

> **Nota (v3.2):** o fluxo **em campo** hoje é o **ESP32** como gateway —
> ver [`esp32/README.md`](esp32/README.md) e
> `cd esp32 && pio run -e gateway_uart -t upload`. As subseções **2 e 3**
> abaixo (ESP‑01 via Arduino IDE) são da geração **v2** e ficam aqui só
> como referência; os arquivos estão em [`legacy/`](legacy/).

### 1. Sensor — Arduino (PlatformIO)

```bash
cd arduino
pio run -e uno -t upload        # placa em /dev/ttyUSB* ou /dev/ttyACM*
```

### 2. Gateway — ESP-01 (Arduino IDE 1.8.x + esp8266 3.1.2) — legado

```bash
cd legacy/gateway
# compilação via linha de comando (opcional):
arduino --verify gateway.ino --board esp8266:esp8266:generic --pref flash=1M64
```

**Procedimento de gravação (sempre):**

| Passo | Gravar | Rodar |
|---|---|---|
| 1 | `GPIO0` → `GND` | retire o `GPIO0` do `GND` |
| 2 | Reconecte o USB (power cycle) | Reconecte o USB (power cycle) |
| 3 | Envie o código (aguarde `SUCCESS`) | o gateway inicia sozinho |

- O chip lê o `GPIO0` **só no boot** — por isso sempre reconectar o USB
- Nunca alimente o ESP-01 com 5 V direto
- Feche o monitor serial antes de gravar

### 3. Teste de hardware — servidor WiFi (opcional) — legado

A pasta [`legacy/wifi_server/`](legacy/wifi_server/) sobe uma página simples que mostra IP,
sinal e o último dado do Arduino — útil para validar o ESP-01 antes de gravar
o gateway:

```
http://<ip-do-esp>
```

## Configuração

O gateway **ativo (ESP32)** lê tudo de `esp32/include/config.h`
(**não versionado**; o `esp32/include/config.example.h` versionado mostra o
modelo):

```bash
cd esp32/include
cp config.example.h config.h    # e edite com seus dados
```

> A tabela abaixo é da geração **v2 (ESP‑01)** e serve de referência; o
> ESP32 usa variáveis equivalentes em [`esp32/include/config.example.h`](esp32/include/config.example.h)
> (inclui também `MQTT_*` e parâmetros de fila/checkpoint).

| Variável (v2, legado) | Descrição |
|---|---|
| `WIFI_SSID` / `WIFI_PASS` | rede **2,4 GHz** (ESP‑01/ESP32 não veem 5 GHz) |
| `PB_TOKEN` | token do Pushbullet (Settings → Account) |
| `DISCORD_URL` | webhook do canal (Editar canal → Integrações → Webhooks) |
| `LOCAL_TXT` / `CEP_TXT` / `UC_TXT` | identificação exibida nas mensagens |

Os mesmos arquivos `config.h` / `config.example.h` da v2 existem em
`legacy/gateway/` e `legacy/wifi_server/`.

## Estrutura do repositório

| Pasta | Status | Descrição |
|---|---|---|
| `arduino/` | ✅ ativo | firmware do sensor (PlatformIO, ATmega328P) — **v3.2** mede a rua pelo ZMPT no A0 e reporta todo pulso |
| `esp32/` | ✅ ativo | gateway ESP32 com RTC interno, fila de mensagens e checkpoint — **v3.2 em campo** (ver [`esp32/README.md`](esp32/README.md)) |
| `docs/` | ✅ | arquitetura, protocolo UART e plano de OTA/qualidade |
| `tools/` | ✅ | logger e leitor da serial |
| `legacy/` | 📦 legado | geração **ESP‑01/v2** preservada como referência (ver [`legacy/README.md`](legacy/README.md)) — `gateway/`, `sensor/`, `esp01/`, `wifi_server/` |

## Histórico de decisões

| Decisão | Motivo |
|---|---|
| IFTTT e Reddit descartados | IFTTT passou a exigir cartão de crédito |
| Discord + Pushbullet | gratuitos, com timestamp auditável |
| MQTT / frame binário com CRC16 | substituído por JSON simples sobre UART |
| Detecção por **presença** | sem depender de calibração de tensão |
| Sem RTC / bateria | o tempo vem do NTP e do próprio Discord |
| No-break | o alerta precisa ser enviado **enquanto** a rua está sem energia |
| **v3: power bank no lugar do no-break** | o no-break não pôde ser mantido |
| **v3.0: ZMPT retirado** | tentativa de detectar a queda pela própria alimentação (Vcc via bandgap) — exigia calibração sem multímetro |
| **v3.1: ZMPT voltou, Vcc removido** | o método pela fonte não media a rua e dependia do pass-through do power bank; o ZMPT dá a resposta direta, sem calibração |
| **v3.2: janela de 55 ms + relatório empacotado** | o v3.1 exigia ~2 s de sinal ausente e **perdia piscar de lâmpada**; agora o Arduino reporta todo pulso medido e o ESP32 decide, embalando várias quedas curtas numa mensagem só |
| **v3.0: ESP32 + RTC** | o roteador cai junto com a rua, então sem NTP durante a queda — o RTC interno carimba o momento e a fila entrega quando a rede volta |
| **v3.0: checkpoint na flash** | cobre o caso do power bank esgotar e tudo desligar |

## Licença

[MIT](LICENSE) — © 2026 Ramiro Heitor Cardoso Fabiano
