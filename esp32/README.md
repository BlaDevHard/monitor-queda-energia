# ⚡ Monitor de Quedas — v3.1 (Arduino + ESP32)

> **v3.1 — o que mudou:** o **ESP-01 foi substituído por um ESP32 com RTC
> interno**, e a detecção voltou a ser **direta na rua, pelo ZMPT101B**.
> A v3.0 tentou detectar a queda pela própria alimentação do Arduino (Vcc
> pelo bandgap) e **voltou atrás** — exigia calibração de limiar e não media
> a rua. Ver [*Por que o Vcc foi removido*](#por-que-o-vcc-foi-removido).

## Como funciona

```
rua ─▶ ZMPT101B ─▶ A0 do Arduino      (o sinal alternada da rede)
                        │
tomada ─▶ power bank (pass-through) ─┬─ USB 1 ─▶ Arduino
                                     └─ USB 2 ─▶ ESP32 (Discord + RTC + fila)

        Arduino TX ─ [1 kΩ] ─┬─▶ ESP32 GPIO16 (RX2)
                             └─ [2 kΩ] ─ GND          divisor 5V→3V3
```

| Evento | O que acontece | Mensagem |
|---|---|---|
| Rua cai | o ZMPT para de ver alternada | **QUEDA** (vermelho) |
| Rua volta | o sinal volta ao A0 | **SERVIÇO ATIVO** (verde) + duração |

O Arduino mede a **amplitude pico-a-pico** do sinal numa janela de ~165 ms
(~10 ciclos de 60 Hz). Não calcula tensão e não mede a própria alimentação —
a rede é medida **direto**, e a alimentação só precisa durar.

Valores medidos nesta instalação (out/2026):

| Cenário | `amp` | `f` |
|---|---|---|
| Rua com energia | ~417–424 | 59,5 Hz |
| Rua sem energia | ~7 | — |
| Limiar (`LIMITE_AMPLITUDE`) | **65** | |

### Por que RTC, fila e checkpoint

O **roteador cai junto com a rua**. No instante da QUEDA não há internet.

| Peça | Papel |
|---|---|
| **RTC interno** | mantém a hora durante a queda, mesmo sem NTP (o system time do ESP32 é calibrado contra o cristal de 40 MHz) |
| **Fila de mensagens** | guarda a QUEDA e sai quando o WiFi volta (1 msg/s para não levar 429 do webhook) |
| **Checkpoint na flash** | se o **power bank esgotar** e tudo desligar, ao voltar o sistema reconstrói o episódio (`QUEDA (recuperada)`) |

> **Sem NTP, a hora é marcada como aproximada** na própria mensagem.
> Melhor dizer "aproximada" do que publicar uma hora inventada.

### Por que o Vcc foi removido

A v3.0 fazia o Arduino ler a **própria Vcc** pela referência de bandgap
interno de 1,1 V, e classificar `tomada` / `bateria` com um limiar
(`VCC_LIMITE_MV`). Três problemas, na prática:

1. **Exigia calibração sem instrumento.** Os dois níveis (tomada e power
   bank) precisavam ser medidos separadamente e o limiar posto no meio —
   e não havia multímetro para isso.
2. **Dependia do power bank assumir limpo.** Pass-through não é garantido;
   alguns bancos reiniciam na troca, gerando um **reboot do Arduino** que
   se confundiria com queda.
3. **Não media a rua.** Era um palpite sobre a rede, indireto. O ZMPT, que
   está lá e mede a rede de verdade, dava a resposta certa sem calibração.

Sobrou o método da v2, que já funcionava em campo. Os campos `fonte` e
`vcc` saíram do protocolo junto.

---

## ⚠️ Verifique ISSO antes de montar

**O power bank mantém a saída USB ligada ENQUANTO está carregando?**

Muitos power banks **desligam a saída USB enquanto carregam**. Se o seu for
assim, o ESP32 — que você quer alimentar pela USB 2 — ficará **desligado
sempre que houver tomada**, e só ligaria durante as quedas. O sistema não
funcionaria.

Teste de 30 s: deixe o power bank na tomada e ligue um celular nele.

- **Carrega normalmente** → ok, pode seguir.
- **Não carrega com a tomada ligada** → alimente o ESP32 por uma segunda
  fonte USB na tomada.

Isso não muda o firmware — é só fiação. (Na v3.0 havia a alternativa de
alimentar pelo pino 5V do Arduino; hoje isso é **evitável e indesejável**:
o ESP32 com WiFi puxa 200–400 mA e passaria pelo PTC de 500 mA da USB do
Uno.)

### Outros riscos reais

- **ZMPT sem energia na entrada:** o sinal cai a ~7 e o sistema registra
  QUEDA — corretamente, porque a rede está realmente sem energia no ponto
  onde o sensor está ligado. Se o ponto de medição não for o correto, o
  alarme será "certo" para o lugar errado.
- **Potenciômetro do módulo ZMPT:** se alguém mexer, o `amp` com rua cai.
  O serial mostra o valor a cada ciclo; se chegar perto de 65, suba.
- **Lado de entrada do ZMPT é REDE (127/220 V).** Não toque nos terminais
  de entrada com a placa alimentada.

---

## Calibração do limiar (sem multímetro)

> **É só software.** Errar o limiar não quebra nada: o pior caso é um alarme
> falso ou perdido, ajustável a qualquer momento pelo serial.

O Arduino imprime a cada ciclo:

```
#ZMPT amp=417 [min=302 max=719] f=59.5Hz  bruto=ENERGIA  conf=3
```

1. `pio device monitor` com a rua **com** energia → anote o `amp`
   (ex.: `417`)
2. Sem energia (ou desligando o circuito com cuidado) → anote o `amp`
   (ex.: `7`)
3. Ponha `LIMITE_AMPLITUDE` **bem abaixo do menor dos dois**, com folga.
   Com 417 e 7, o limiar `65` dá folga dos dois lados.

O `f=` só aparece quando há sinal. Se ele bater **~60 Hz**, é rede de
verdade; se sortear valores a cada leitura, é ruído — o teste mais rápido
para dizer sinal de rua de tremulação.

Os valores ficam em `arduino/src/main.cpp`:

```cpp
#define LIMITE_AMPLITUDE 65   // amp >= 65 -> presente
#define CONFIRMACOES 3        // debounce
#define AMOSTRAS 1500         // janela ~165 ms (~10 ciclos de 60 Hz)
```

---

## Compilar e gravar

```bash
# --- sensor (Arduino) ---
cd ~/Documentos/monitor/arduino
pio run -e uno -t upload          # /dev/ttyACM0

# --- gateway (ESP32) ---
cd ~/Documentos/monitor/esp32
pio run -e gateway_uart -t upload # upload via USB, auto-reset
pio device monitor                # 115200
```

Upload do ESP32 via USB normal — **sem a dança do `GPIO0`** do ESP-01.

### Arduino — protocolo (9600 8N1)

```
{"boot":"Sensor OK","versao":"3.1"}
{"energia":"presente","amp":417}
{"energia":"ausente","amp":7}
```

`energia` mantém o contrato desde a v2. O Arduino também manda linhas de
diagnóstico (`#ZMPT ...`), que o ESP32 ignora.

---

## Configuração

```bash
cd esp32/include
cp config.example.h config.h   # e edite com seus dados
```

| Variável | Descrição |
|---|---|
| `WIFI_SSID` / `WIFI_PASS` | rede **2,4 GHz** |
| `PB_TOKEN` / `DISCORD_URL` | token Pushbullet e webhook do Discord |
| `PINO_RX_ARDUINO` | pino da UART2, padrão `16` |
| `FILA_MAX` | mensagens guardadas com a rede fora, padrão `32` |
| `CHECKPOINT_MS` | periodicidade do checkpoint na flash, padrão `60000` |
| `SENSOR_SILENCIO_MS` | silêncio do Arduino antes do alerta, padrão `45000` |
| `MQTT_ATIVO` / `MQTT_BROKER` | broker MQTT (base do app Android) |
| `MODO_MUDO` | `1` = bancada: nada sai para Discord/Pushbullet/MQTT; a mensagem aparece no serial |
| `DOCS_URL` | caminho da documentação, publicado no `SISTEMA ONLINE` |
| `DOCS_REPUBLICA_MS` | intervalo de republicação do link da documentação, padrão `604800000` (7 dias) |

> `config.h` **não é versionado** — WiFi, tokens e identificação ficam fora do git.

### Documentação no Discord

A mensagem `SISTEMA ONLINE` publica o `DOCS_URL`. O link volta sozinho para o
canal a cada `DOCS_REPUBLICA_MS` (7 dias): o Discord rola a conversa, e o link
do boot sai de vista rápido. A republicação passa pela mesma fila de mensagens
do resto — se a rede estiver fora quando vencer o prazo, ela sai junto com o
próximo evento assim que o WiFi voltar.

---

## Consumir o estado

```bash
# HTTP
curl http://<ip-do-esp>/estado

# MQTT (estado retido: chega mesmo sem ter visto a transição)
mosquitto_sub -h broker.emqx.io -t 'monitor/energia/estado' -v
mosquitto_sub -h broker.emqx.io -t 'monitor/energia/status'  -v
```

```json
{"energia":"ausente","amp":7,"em_queda":true,"sinal_saturado":false,
 "desde":"15/10/2026 15:34:22","duracao_s":119,"ultima_queda_s":0,
 "fila":1,"hora_conf":true,"uptime_s":3600,"wifi_rssi":-58,
 "ip":"192.168.1.9","firmware":"gateway_uart"}
```

`hora_conf:false` significa que a hora ainda não veio do NTP.

---

## Limitação conhecida

O instante da **QUEDA** é exato (o ESP32 está vivo, o RTC corre, o WiFi caiu
depois). O instante do **RETORNO** inclui o tempo de boot do roteador — a
mensagem só sai quando o WiFi volta, então o horário registrado fica atrasado
em 30–90 s.

**Correção:** levar o **roteador e o modem para o power bank** junto com os
demais. Aí não há atraso nenhum e a mensagem sai em tempo real.

---

## Autonomia no blecaute

Power bank de **10 000 mAh** (37 Wh; ~6 300 mAh disponíveis a 5 V após o
boost a ~85%):

| Cenário | Corrente | Autonomia |
|---|---|---|
| Normal (WiFi ligado, ocasionais publicações) | ~175 mA | **~36 h** |
| **Rua caiu, roteador morto** (ESP32 varrendo canais) | ~310 mA | **~20 h** |

Um banco "10 000 mAh" barato costuma entregar 5 000–6 500 mAh na USB
(roubo de fabricante) — nesse caso, ~10 h no pior caso. Bancos ruins
desligam abaixo de ~50 mA de carga; a nossa, ~175 mA, está seguro.

---

## Estrutura

| Arquivo | Função |
|---|---|
| `arduino/src/main.cpp` | **sensor v3.1** — mede o ZMPT no A0 por amplitude |
| `arduino/src/main_v1.cpp` | a v1 original, preservada do commit inicial |
| `esp32/platformio.ini` | `gateway_uart` (ativo) e `unificado_adc` (legado) |
| `esp32/src/monitor_core.*` | WiFi, NTP/RTC, Discord, Pushbullet, MQTT, HTTP, fila, checkpoint |
| `esp32/src/relogio.*` | fonte de tempo: NTP → RTC interno → checkpoint |
| `esp32/src/main_uart.cpp` | gateway UART2 (ativo) |
| `esp32/src/main_adc.cpp` | **legado** — leitura direta do ZMPT no ADC do ESP32 |
| `esp32/include/config.example.h` | modelo versionado |

---

## Pendências

1. ~~**Testar queda de ponta a ponta**~~ — **feito em 2026-10-15**. A sonda
   `arduino/src/main_ciclo_teste.cpp` (`pio run -e ciclo_teste`) simula
   `presente → ausente → presente` sem derrubar a rua. Rodou 3 ciclos: QUEDA e
   RETORNO com início, retorno e duração corretos (20 s), amplitudes 7 e 421.
2. **Verificar pass-through do power bank** (bloco *Verifique ISSO*)
3. **Roteador no power bank** — elimina o atraso do RETORNO e permite
   enviar a QUEDA em tempo real
4. **Power bank esgotado** — validar o caminho `QUEDA (recuperada)`
5. **Broker próprio** (Mosquitto) para produção — EMQX é público
6. **App Android** — consumir MQTT retido + HTTP local

### Verificado em bancada (2026-10-15)

- Ciclo `QUEDA → SERVIÇO ATIVO` (item 1 acima), 3 voltas.
- `MODO_MUDO` desligado e `MQTT_ATIVO` ligado: `SISTEMA ONLINE` sai de verdade.

### Legado

- **ESP-01** — substituído pelo ESP32. O `gateway/gateway.ino` continua no
  repo como referência do sistema publicado em
  [BlaDevHard/monitor-queda-energia](https://github.com/BlaDevHard/monitor-queda-energia).
- **Detecção pela Vcc (v3.0)** — tentada e removida. O código não está mais
  em `main.cpp`; o histórico está no git e a motivação em
  [*Por que o Vcc foi removido*](#por-que-o-vcc-foi-removido).
