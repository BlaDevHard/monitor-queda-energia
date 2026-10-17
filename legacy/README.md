# Legado — geração ESP‑01 (v1/v2)

Estes diretórios são a **geração anterior** do monitor de quedas, baseada no
**ESP‑01 (ESP8266)**. Eles **não** são a versão em campo — a produção
atual é a **v3.2**, que vive em [`../arduino/`](../arduino/) (sensor) e
[`../esp32/`](../esp32/) (gateway).

Ficam aqui **preservados como referência e histórico**, fora da raiz para
não poluir o diretório ativo. O `git` mantém o histórico completo
(`git log --follow <arquivo>`).

| Pasta | O que era | Sucessor |
|---|---|---|
| `gateway/` | firmware do gateway ESP‑01 (`.ino`, Arduino IDE) — v2 que esteve em campo (Discord + Pushbullet + NTP) | [`../esp32/`](../esp32/) com `gateway_uart` |
| `sensor/` | sensor v2 (ATmega), com experimentos IFTTT / MQTT / frames binários | [`../arduino/`](../arduino/) com env `uno` (v3.2) |
| `esp01/` | projeto PlatformIO do gateway ESP‑01 | [`../esp32/`](../esp32/) |
| `wifi_server/` | servidor web de **teste** no ESP‑01 (mostra IP/sinal para validar o hardware) | sem equivalente direto (o ESP32 tem `WebServer` embutido) |

## Por que saíram da raiz

1. **São a v2.** A v3 substituiu o desenho: ZMPT medindo a rua (não mais
   detecção pela Vcc/power bank), ESP‑01 → **ESP32 com RTC interno**, fila
   de mensagens e checkpoint na flash.
2. **Reduzem ruído.** Com 4 diretórios legados na raiz, dá para compilar o
   firmware errado por engano e a estrutura fica ambígua (`esp32/` = gateway,
   mas já houve `esp01/` = gateway também).
3. **Ferramentas diferentes.** `gateway/` e `wifi_server/` são `.ino` de
   Arduino IDE; o resto é PlatformIO. Misturava dois fluxos de build.

## Aviso

- **Não versionar `config.h`.** Já está no `.gitignore` (`**/config.h`).
  Os `config.h` destas pastas contêm WiFi/Discord/Pushbullet reais e não
  devem subir.
- Estes firmwares **não** recebem mais correções. Bugs conhecidos da v2
  (ex.: atraso do RETORNO pelo boot do roteador) foram resolvidos na v3
  via fila + RTC, não por correção aqui.

Para o fluxo atual, ver [`../README.md`](../README.md) e
[`../docs/arquitetura.md`](../docs/arquitetura.md).
