# Arquitetura

## Visão geral

Pipeline de 2 estágios conectados por UART:

```
┌────────────────────┐   UART 9600    ┌────────────────────┐   HTTPS
│     ATmega328P     │ ─────────────▶ │   ESP-01 (WiFi)    │ ─────────▶ Discord
│   ZMPT101B → A0    │  linhas JSON   │      gateway       │           Pushbullet
│ detecta PRESENÇA   │                │  estado + hora NTP │
└────────────────────┘                └────────────────────┘
      (sensor)                              (gateway)
```

## Estágios

### 1. Sensor — ATmega328P (Arduino Uno)

- **Hardware**: Arduino Uno, ZMPT101B conectado em A0
- **Função**: amostra o ADC por janelas de ~165 ms (~10 ciclos de 60 Hz) e
  compara a amplitude pico-a-pico com o limiar `LIMITE_AMPLITUDE` (65 contagens)
- **NÃO calcula tensão** — decide apenas *presença* ou *ausência* de sinal
- **Debounce**: só altera o estado após `CONFIRMACOES` (3) leituras iguais
  seguidas (~2 s), evitando alarmes por interferência
- **Saída**: UART 9600 8N1, uma linha JSON por mensagem (ver
  [protocolo-uart.md](protocolo-uart.md))
- **LED**: aceso = QUEDA (sem serviço)

### 2. Gateway — ESP-01 (ESP8266)

- **Função**: ler a UART, manter o estado da rede e publicar eventos
- **Justificativa**: o ATmega não tem WiFi; o ESP-01 é barato e resolve só essa ponta
- **Conectividade**: estação (STA) na rede 2,4 GHz
- **Hora**: NTP (`pool.ntp.org`, GMT-3) — sem RTC e sem bateria
- **Destinos**:
  - Discord (webhook) — log auditável; o próprio Discord carimba a data/hora
  - Pushbullet (token) — notificação instantânea no celular
- **Segredo**: WiFi/tokens/identificação ficam em `config.h` (**não versionado**)

## Máquina de estados

```
                3 leituras "ausente" seguidas (~2 s)
   PRESENTE ─────────────────────────────────────────▶ AUSENTE
      ▲                                                  │
      │        3 leituras "presente" seguidas (~2 s)     │
      └────────────────────────────────────────────────  │
                                                         ▼
              publica: QUEDA (vermelho)     publica: SERVIÇO ATIVO (verde)
                                              + duração da interrupção
```

- **1ª leitura após o boot** publica o estado atual (`QUEDA` ou `SERVIÇO ATIVO`)
  junto com a mensagem `SISTEMA ONLINE`
- Sem mensagens repetidas enquanto o estado não muda

## Fluxo de dados

1. Sensor abre uma janela de ~165 ms e mede amplitude pico-a-pico em A0
2. Compara com o limiar (65) → leitura bruta `presente` / `ausente`
3. Debounce: confirma o novo estado só após 3 leituras idênticas
4. Envia linha JSON por UART (~665 ms entre linhas)
5. Gateway lê a linha e aplica a máquina de estados
6. Em transição (ou na 1ª leitura), envia mensagem para Discord e Pushbullet
7. Aguarda ~2 s (HTTPS) e volta a ler a UART (o buffer do ESP absorve as linhas
   acumuladas nesse meio-tempo)

## Alimentação e no-break

```
no-break ──▶ roteador (WiFi)
        └──▶ Arduino Uno ── 5V ── AMS1117-3.3 ──▶ ESP-01 (VCC + CH_PD)
                    └──▶ ZMPT101B (VCC)
```

- Enquanto o no-break aguenta, **todo o sistema continua vivo** e o alerta é
  enviado normalmente durante a queda
- Se a bateria acabar, quando a energia voltar tudo dá boot e o gateway
  publica o estado atual — o log no Discord não fica com buraco
