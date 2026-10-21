# Testes de campo — 18/10/2026 (noite)

> Firmware `gateway_uart`, slot `app1`, `ota_estado=VALIDA`, K_VRMS 0,2987.
> Feitos na véspera da validação de campo que começou às 18:22:31.
> Evidência bruta: `data/testes-campo-2026-10-18/`.

Três testes, mais um não planejado. O aparelho saiu de todos com o mesmo
firmware e sem uma única correção.

---

## 1. Queda simulada pelo ZMPT (planejado)

**Como:** desplugar a **entrada AC** do módulo ZMPT101B da tomada. A parte DC
(VCC/GND) e o Arduino continuam ligados, então o ESP32 e o roteador ficam de
pé — o que isola a detecção do blecaute.

**Resultado: funcionou, com duas fases distintas.**

### 1.1 Filtro de rajada (19:32:59 – 19:33:05)

```
19:32:59.281  EVENTO: QUEDA (amp=16) [adiado 8 s]
19:33:01.229  EVENTO: SERVICO ATIVO (queda de 2s) [cancelado — virou relatorio]
19:33:02.470  EVENTO: QUEDA (amp=17) [rajada — vai no relatorio]
19:33:03.119  EVENTO: SERVICO ATIVO (queda de 0s) [rajada]
19:33:04.382  EVENTO: QUEDA (amp=12) [rajada — vai no relatorio]
19:33:04.682  EVENTO: SERVICO ATIVO (queda de 0s) [rajada]
19:33:09.718  Relatorio de pulsos: 3 eventos (1 quedas, 2 flicker)
```

**Seis transições, zero mensagens avulsas, um relatório só.** A primeira
foi de 2 s — o tempo exato que a v3.1 usava como limiar — e **não publicou**,
porque a grancia de 8 s ainda estava correndo. É a decisão de design da v3.2
provada com um plugue: sem ela, um blecaute de 5 s com 3 quedas e 3 retornos
viraria 6 mensagens.

### 1.2 Blecaute confirmado (19:33:21 – 19:33:36)

```
19:33:21.949  EVENTO: QUEDA (amp=15) [adiado 8 s]
19:33:29.834  QUEDA confirmada como blecaute real — publicando alerta
19:33:36.505  EVENTO: SERVICO ATIVO (queda de 15s)
```

| Métrica | Alvo | Medido |
|---|---|---|
| Gracia | `QUEDA_GRACIA_MS` 8000 ms | **7,885 s** |
| Duração reportada | — | 15 s (real: 14,6 s) |
| Amplitude na rua morta | — | 12–17 |
| Amplitude com rua | — | 415–424 |

**O limiar tem 25× de folga** para queda real: `amp` cai de ~420 para ~15,
e o limiar de produção está em **65** (escala de 10 bits do Arduino).

---

## 2. Corte de energia do ESP32 (planejado)

**Como:** desligar o power bank que alimenta a placa, ~22 s morto, religar.

| Verificado | Esperado | Observado |
|---|---|---|
| Morreu de verdade | `uptime` volta a 0 | **924 → 4** ✅ |
| Mesmo firmware | `slot` = `app1` | `app1` ✅ |
| **Sem rollback espúrio** | `ota_estado` = `VALIDA` | `VALIDA`, `pend=False` ✅ |
| NTP resincroniza | `hora_conf` false → true | `False → True` ✅ |
| Fila esvazia | `fila` 1→0 | **2 → 0** ✅ |
| Volta a operar | HTTP responde | ping 0% de perda ✅ |

Duas mensagens enfileiradas no boot (`SISTEMA ONLINE` + `SERVIÇO ATIVO`),
porque o DNS ainda não estava pronto. Entregues assim que a rede veio —
1 mensagem por passo, para não levar 429 do webhook.

**3,29 h depois:** boot 19:42:36, `slot app1`, `VALIDA`, **nenhuma reversão
de firmware**. O `otadata` sobreviveu ao corte limpo.

---

## 3. Reboot não planejado do ESP32 (19:26:39)

**Foi erro do operador, e virou teste.** A captura de serial usava a linha
de auto-reset do CH340:

```python
s.setRTS(True); time.sleep(0.1); s.setRTS(False)   # ❌ reinicia o ESP32
```

No CH340 o **RTS é o pino EN**. Essa sequência serve para flashar — e derrubou
o monitor em produção. **Corrigido em `testes/observar_serial.py`:**
abrir a porta e desligar `DTR` e `RTS`. Para observar, usar **só HTTP**.

O lado bom: o aparelho se recuperou sozinho e **testou a fila sem ninguém pedir**.

```
19:26:45  Sem rede: na fila (1/32) — SISTEMA ONLINE
19:26:48  Discord: 204 → Fila: enviada — SISTEMA ONLINE
19:27:06  Relogio: NTP sincronizado apos boot (hora confiavel)
19:27:06  Sem rede: na fila (1/32) — SERVICO ATIVO
19:27:08  Discord: 204 → Fila: enviada — SERVICO ATIVO
```

E o checkpoint voltou certo (`Checkpoint: servico ativo desde 18/10/2026 18:22:40`).

---

## 4. Achado: o `desde` mente depois de um corte de energia

Depois do power-cycle o `/estado` mostrava `desde=19:41:45` com o boot às
**19:42:37** — 52 s de diferença.

**Causa**, em `src/relogio.cpp`:

```cpp
time_t relogioAgora() {
  if (ntpRespondeu()) return time(nullptr);
  if (baseEstimada > 0) return baseEstimada + (millis() - baseMillis) / 1000UL;
  return 0;
}
```

A estimativa **soma o tempo de uptime** — assume que a placa nunca ficou
desligada. Passou um corte de 52 s e a estimativa fica 52 s atrasada.

O `horaMudanca` (o `desde` do `/estado`) é carimbado **no boot**, com essa
estimativa, e **não é corrigido quando o NTP chega** alguns segundos depois.
Fica errado até a próxima mudança de estado.

O firmware não é ingênuo quanto a isso — `notaHora()` emite "⚠️ hora
aproximada (sem NTP no momento)" — mas só nas mensagens, não no `desde`.

**Correção proposta** (não aplicada — ver §6): ao sincronizar o NTP,
re-carimbar `horaMudanca` **quando não houver queda em curso**. Numa queda
em andamento o início tem que permanecer onde está.

---

## 5. Achado corrigido: o limiar **não** está em risco

Às 19:40 o `amp` ficou oscilando entre **397 e 407** com o leitor mostrando
limiar 400 em `include/config.h`. Concluí às pressas que um brownout de
119 V geraria falso alerta. **Estava errado.**

`LIMITE_AMPLITUDE 400` pertence a `src/main_adc.cpp`, e esse arquivo é
**excluído** do firmware em execução:

```ini
[env:gateway_uart]
build_src_filter = +<*> -<main_adc.cpp>
```

O `400` é do `unificado_adc` (variante em que o ESP32 lê o ZMPT direto),
que **não está no ar**. Quem classifica é o **Arduino**, com
`LIMITE_AMPLITUDE 65` — escala de 10 bits dele. Na execução real:
**65** o limiar, **415–424** com rua, **7–17** sem rua. Folga de 6×.

**O dado já tinha provado isso e eu não tinha ligado:** `amp` ficou em
397/398 repetidas vezes sem nada acontecer, enquanto `amp=12–17` disparou na
hora. Se o limiar fosse 400, teria tripado.

### Lição de método

Um `grep` que acha um `#define` não diz se aquele `#define` **está no
binário que está rodando**. Antes de agir sobre um valor de configuração,
conferir o `build_src_filter` do environment ativo — e, se possível,
**contraditar com o dado observado** antes de propor correção.

---

## 6. O que não foi feito (e por quê)

**Nenhum deploy foi feito.** Com o achado do §5 descartado, restava apenas
o re-carimbo do `desde` (§4), que é cosmético — e o preço de um deploy é
**zerar a validação de campo** que já tinha rendido fila, restore de
checkpoint, power-cycle, filtro de rajada e blecaute medido.

Se um dia for aplicar, vale juntar no mesmo deploy a **subtask 2.6**
(limiar de sub-tensão distinto de `QUEDA`, abaixo de 0,9×V_NOM = 114,3 V),
para não pagar o custo da validação duas vezes.

**A calibração também não avançou** — ver `calibracao-zmpt.md` §8. A
variação natural apareceu (121,7 → 124,5 V em 5 min), mas as leituras do
multímetro chegaram fora da janela de captura, e casar errado produz uma
divergência de `k` que não é distinguível de um K realmente errado.
**A hipótese de K errado fica ABERTA.**

---

## 7. Arquivos

| Arquivo | Conteúdo |
|---|---|
| `data/testes-campo-2026-10-18/serial.log` | log serial dos testes |
| `data/testes-campo-2026-10-18/estado.log` | transições de `/estado`, teste do ZMPT |
| `data/testes-campo-2026-10-18/powercycle.log` | transições do corte de energia |
| `data/testes-campo-2026-10-18/amostras.tsv` | ~3 amostras/s do `/estado`, com timestamps |
| `testes/amostrar.py` | janela de N s → `amp_media`, `sigma`, `min`, `max`, `n` |
| `testes/continuo.py` | amostrador contínuo com carimbo (para parear com o multímetro) |
| `testes/observar_serial.py` | captura serial **sem** auto-reset |
| `testes/powercycle.py` | observa corte de energia só por HTTP |
| [`calibracao-zmpt.md`](calibracao-zmpt.md) | §8 — a sessão noturna e o protocolo de sincronia |
