# ESP32 — Update: como está × como vai ficar

> **Mapa mental da atualização do ESP32** (prioridade 1 do
> [`plano-ota-qualidade.md`](plano-ota-qualidade.md)).
> Firmware de referência: **v3.2** (`a6db7ef`), gravado em campo via
> `pio run -e gateway_uart -t upload`.
> Arquivos inspecionados: `esp32/src/monitor_core.cpp` (abertos no VS Code,
> conforme diretiva da sessão 17/10).

---

## 1. Tabela geral — estado atual × mudança proposta

| Aspecto                       | Como está (v3.2 em campo)                                                                                       | Como vai ficar (update)                                                                                                                | Risco         | O que o risco quer dizer                                                                                                                                                                                                                                                                                                                                                                                                              |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Servidor HTTP — lifecycle** | `servidor.begin()` chamado **1×** em `coreIniciar()` (L925). `handleClient()` só roda com `wifiOk` (L1105–1107) | `servidor.begin()` re-chamado na transição `!wifiOk → wifiOk` em `coreLoop()` (L979–981) — **subtask 1.1**                             | 🟢 **Baixo**  | É uma linha nova num ponto já isolado (`if (!wifiOk)`), que só roda quando hoje já está tudo quebrado. Pior caso: HTTP continua morto, igual já está. Não toca em detecção nem em mensagem nenhuma.                                                                                                                                                                                                                                   |
| **HTTP após blecaute**        | ❌ **Bugado**: `WiFi.mode(WIFI_OFF)` (L976) destrói o socket de escuta → *connection refused* até reboot manual  | ✅ Volta a responder `/` e `/estado` após cada reconexão                                                                                | 🟢 **Baixo**  | Mesma troca da linha acima: só melhora o que já está falho. Não há estado bom a perder — hoje, depois do primeiro blecaute, o HTTP já não funciona mais até reiniciar.                                                                                                                                                                                                                                                                |
| **Atualização de firmware**   | Só por USB (`pio run -e gateway_uart -t upload`) — exige desplugar e acesso físico                              | **OTA** via `ArduinoOTA` ou `WebServer`+`Update` (escolha na 1.2) — atualiza sem tocar na placa                                        | 🔴 **Alto**   | É o risco central de todo o projeto. OTA escreve na **flash do ESP32 instalado lá em campo** (power bank, atrás da tomada, difícil de alcançar). Se a gravação falhar no meio — WiFi cai, energia cai, pacote corrompido — o chip pode ficar sem firmware válido (tijolo) e a única recuperação é desplugar e regravar por USB. Por isso: gate de segurança, MD5, rollback do bootloader e **teste em bancada antes de campo** (1.6). **Mitigação em §2b** — resumo: a gravação vai no slot inativo (`app1`), então falhar nunca danifica o firmware que está rodando (`app0`). |
| **Partição flash**            | `default.csv` — `app0(ota_0)` + `app1(ota_1)` + `otadata`; firmware atual **986.480 B** em slots de 1.310.720 B | **Sem mudança** — já é dual-slot, OTA é só software                                                                                    | 🟢 **Nenhum** | Não muda nada físico nem de build: a partição `default.csv` **já** traz os dois slots e o `otadata`. O firmware atual já cabe com folga (986 KB dos 1.310 KB). Zero risco porque zero mudança.                                                                                                                                                                                                                                        |
| **Rollback**                  | Inexistente — erro de gravação = tijolo até acesso físico                                                       | Bootloader do ESP32: app que não sobrevive a 2 boots volta ao slot anterior; `Update.end()` só com MD5 OK                              | 🟢 **Baixo**  | É a **mitigação** do risco alto de cima, não um risco novo. O bootloader do ESP32 tem watchdog de boot: se o app novo não sobrevive a 2 inicializações, ele volta sozinho ao slot antigo. Ou seja, mesmo falhando o OTA, o aparelho se recupera sem toque físico.                                                                                                                                                                     |
| **Gate de segurança OTA**     | n/a                                                                                                             | Só inicia OTA com `emQueda==false && wifiOk` (nunca durante blecaute); senha em `ArduinoOTA.setPassword`; **nunca** exposto à internet | 🟡 **Médio**  | Médio porque depende de disciplina de operação, não de código complexo: o gate é uma condição no firmware, mas o erro humano (disparar OTA com rua oscilando, expor o serviço à internet) é sempre possível. Consequência de falha aqui é a mesma do risco 🔴 — gravação interrompida.                                                                                                                                                |
| **Transporte/infra**          | Sem mDNS — IP vem do DHCP (muda, DHCP reserva é prerequisite)                                                   | **1.2:** escolher `ArduinoOTA` (descobre por mDNS → resolve o IP DHCP) **ou** `POST /update` via `Update`                              | 🟡 **Médio**  | Decisão de arquitetura com trade-off real: `ArduinoOTA` resolve o problema do IP que muda (descobre o device por nome na rede), mas é uma dependência a mais no fluxo de build/upload; `POST /update` é mais simples e explícito, mas exige IP fixo (reserva DHCP). Escolher errado só custa retrabalho — não danifica o aparelho.                                                                                                    |

---

## 2. O bug HTTP em detalhe (subtask 1.1)

Cadeia de causalidade no `monitor_core.cpp` atual:

```
blecaute (roteador morre com a rua)
  └─ coreLoop() → espera reconexão com rádio DESLIGADO p/ poupar bateria
       └─ WiFi.disconnect(true) + WiFi.mode(WIFI_OFF)      [L975–976]
            └─ stack TCP inteiro destruído, inclusive o socket de escuta
                 do WebServer (aberto 1× no boot, L925)
  └─ rua volta → WiFi reconecta, wifiOk = true              [L979–981]
       └─ handleClient() atende em socket morto → connection refused
            → HTTP morto até reboot manual (e OTA também, hoje)
```

**Fix proposto** — L980, dentro do `else` (WiFi conectado):

```cpp
} else {
  if (!wifiOk) {
    Serial.println("WiFi: voltou");
    servidor.begin();   // reabre o socket de escuta destruído pelo WIFI_OFF
  }
  wifiOk = true;
}
```

O `if (!wifiOk)` garante **uma** chamada por transição (a cada blecaute),
não a cada passo do loop.

---

## 2b. Mitigação do risco 🔴 — por que "tijolo" é improvável

**Ideia-chave: o OTA nunca sobrescreve o firmware que está rodando.** A
partição `default.csv` tem **dois slots**. A gravação vai para o slot
**inativo** (`app1`); o app atual continua intacto rodando de `app0` durante
todo o processo. O `otadata` (que diz qual slot bootar) só muda no final, se
`Update.end()` confirmar tudo. Interromper a gravação → `app1` fica
incompleto, `app0` continua perfeito → **nada muda no aparelho**.

"Tijolo" só existiria se o `otadata` apontasse para um `app1` corrompido —
e as camadas abaixo impedem exatamente isso:

| # | Camada | Mecanismo | O que neutraliza |
|---|---|---|---|
| 1 | **Gate pré-OTA** | Só inicia com `emQueda==false && wifiOk` e WiFi estável (fora da escada de reconexão) | Começar gravação em momento de queda provável |
| 2 | **Slot inativo** | Grava em `app1`, roda de `app0` (nativo do bootloader ESP32) | Interromper a gravação nunca danifica o firmware em execução |
| 3 | **MD5 ponta a ponta** | Host calcula hash do `.bin`; `Update` compara com o hash do header; `Update.end()` só confirma se bate → senão `Update.abort()` | Pacote corrompido no transporte WiFi |
| 4 | **Rollback do bootloader** | Novo app marcado "pendente"; se não sobrevive a **2 boots** (crash antes de `esp_ota_mark_app_valid_cancel_rollback()`), volta sozinho ao slot antigo | Firmware que compila mas trava no boot — recuperação **automática**, sem toque físico |
| 5 | **Nunca na internet** | OTA só em rede local + senha (`ArduinoOTA.setPassword`) | Externos disparando gravação |
| 6 | **Bancada primeiro (1.6)** | Mesmo `.bin` e mesmo fluxo testados num ESP32 de mesa | Qualquer problema de integração aparece com acesso físico trivial |

**Residual (quase teórico):** `otadata` aponta p/ `app1`, app novo passa 2
boots e só **depois** corrompe (ex.: defeito de flash) → exigiria USB.
Por isso o rollback dispara **cedo** (2 boots): o app novo tem pouco espaço
para "sobreviver falso". Seguro operacional final: **nunca fazer OTA sem
estar em casa por algumas horas**.

---

## 3. O que NÃO muda

| Item | Motivo |
|---|---|
| Detecção de quedas/pulsos (janela 5 s, empacotamento) | Produto validado em campo — intocável nesta fase |
| Protocolo UART Arduino→ESP32 (JSON texto) | v3.2 estável; não faz parte do escopo OTA |
| MQTT / Discord / Pushbullet / fila / checkpoint | Independentes do update; consumidos como estão |
| Partição flash (`default.csv`) | Dual-slot já existe — nenhuma repartiçãoção |
| Env de build (`gateway_uart`) e pinagem | Mesmo alvo de upload, agora com etapa OTA |
| Arduino Uno (`arduino/`, env `uno`) | OTA do Arduino é prioridade 5 separada e **experimental** — falta fio ESP32 TX→Arduino RX + auto-reset |

---

## 4. Sequência do update (do plano, prioridade 1)

| Passo | Conteúdo | Status |
|---|---|---|
| **1.1** | Re-chamar `servidor.begin()` na reconexão WiFi | ✅ **validado em bancada** (18/10) |
| **1.2** | Escolher transporte: `ArduinoOTA` × `WebServer`+`Update` | pendente |
| **1.3** | Integrar `Update.begin(WRITE_FLASH)/write()/end()` + MD5 | pendente |
| **1.4** | Gate: só com rua ligada (`emQueda==false && wifiOk`) | pendente |
| **1.5** | Rollback + log do slot ativo no Discord | pendente |
| **1.6** | Teste em **bancada** antes de campo | ✅ 4/4 (boot · horária · queda · HTTP pós-blecaute) |

> **Extra neste update (prioridade 3 do plano):** relatório horário de
> saúde — `SERVIÇO PERMANECE ATIVO` a cada `HORARIA_MS` (60 min) do
> **último envio** (não de relógio de parede: no blecaute não há WiFi;
> quando a rede volta, o relatório sai sozinho após a fila de alertas e
> vale como **confirmação de retorno**). Suprimido durante queda.
> Ver [[arquitetura]] (box "Hora aproximada") sobre o `⚠️` de hora sem NTP.

---

## 6. Registro do upgrade v3.2 → v3.3 (18/10/2026)

Aplicado ao vivo com o fluxo de [[01-fluxo-firmware]] (nota didática no
vault ASCENE).

| Estágio | Resultado |
|---------|-----------|
| **1. Versão** | 3 commits assinados: `eeeea5b` (fix) · `d3d1960` (feat) · `dcbd36f` (docs) |
| **2. Bancada** | `MODO_MUDO=1` + `HORARIA_MS=60000` → **checklist 4/4** |
| **3. Gravação** | Produção (`MODO_MUDO=0`, 60 min) — autorizada pelo usuário; Discord 204 ✅ |
| **4. Validação** | Em curso — 7–14 dias (ver abaixo). **Reiniciada 14:32** (ver §6b) |
| **5. Publicação** | ✅ push assinado `3128237..9b870d2` (5 commits). Tag `v3.3` **só após** validar |

**Prova do fix em bancada:** queda de WiFi → escada 10/20/40 s → reconexão
(`.4`→`.5`) → `GET /estado` = **HTTP 200** com `uptime_s: 902` (mesmo boot,
**sem reboot**). Bônus: queda real de ~4 s durante o religamento foi
empacotada em relatório, sem alerta falso.

### 6b. Segunda gravação: `vrms` + `v_nom` (18/10, 14:32)

Mesmo dia, ~1 h depois da primeira: **v3.3 + calibração do ZMPT.**
- `vrms = K_VRMS × amp` no `/estado` **e** no MQTT (+ `v_nom`).
  `vrmsAtual()` devolve **0** quando `amp < 10` — sem rua o `amp` é só
  ruído do ADC (~7); em vez de inventar "2,1 V", devolve 0.
- **`K_VRMS = 0.2976`** (ponto 1). A regressão de 5 pontos deu **0,2987**;
  delta 0,37% — **mantido** por decisão (menor que a incerteza do
  multímetro). Corrigir na próxima gravação. Ver [`calibracao-zmpt.md`](calibracao-zmpt.md).
- **Validação de campo reiniciou às 14:32** (contagem 7–14 dias do zero).
  Aceito pelo usuário: a mudança é **aditiva** — não toca detecção, fila,
  horária nem o fix do HTTP, que continua testado.
- **Não foi por OTA:** não existe código OTA ainda (subtasks 1.2–1.6
  pendentes). Gravado por USB (`pio run -t upload`). O fix 1.1 apenas
  *destrava* o caminho do OTA futuro.

**Validação de campo (a acompanhar — reiniciada 18/10, 14:32):**
- [ ] ~24 horários/dia no Discord, sem duplicata
- [ ] Primeira queda **real** → QUEDA → fila → horária de retorno
- [ ] Blecaute longo (>1 h) → horária não se perde
- [ ] HTTP pós-blecaute responde sem reboot (confirmar em campo)
- [ ] `uptime` só cresce (sem reboot misterioso)
- [ ] `vrms` coerente com o multímetro (±5%) em horários distintos

📌 **19/10 (segunda): medições de manhã, tarde e noite** para abrir a faixa
da calibração (span hoje só 4,7%) — ver [`calibracao-zmpt.md`](calibracao-zmpt.md) §7.

### Achados a corrigir (não bloqueiam)
- **`amp` duplicado no JSON** de `/estado` (`"amp":429` aparece 2×) — bug
  cosmético, clientes leem o primeiro. Corrigir na próxima janela.
- **IP muda por DHCP** (`.4`→`.5`) — confirma a **reserva DHCP** como
  prerequisito do OTA; reforça `ArduinoOTA` (mDNS) na decisão 1.2.

**Aceite (do plano):** `pio run` gera `firmware.bin`; uma requisição grava em
`app1`, `otadata` aponta para o novo slot, device reboota rodando a nova
versão **sem** tocar na placa.

---

## 5. Prerequisites de ambiente (não são código)

- [ ] **Reserva DHCP** do IP do ESP32 no roteador (hoje o IP muda; barra o
      HTTP por IP e o transporte por `POST /update`)
- [ ] Rua ligada em algum momento do dia (OTA nunca durante blecaute)
- [ ] `git config commit.gpgsign true` (commits da sessão saíram com `-S` manual)

---

## Links

- [[plano-ota-qualidade]] — plano completo (prioridades 1–7)
- [[arquitetura]] — visão geral do sistema
- [[protocolo-uart]] — protocolo Arduino → ESP32 (v3.2)
- [`esp32/src/monitor_core.cpp`](../esp32/src/monitor_core.cpp) — código
  inspecionado (aberto no VS Code)
- [[README]] — README principal do projeto
