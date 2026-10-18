# Plano — OTA, Qualidade de Energia e Acompanhamento

**Projeto:** Monitor de Quedas de Energia (Antonina‑PR)
**Data:** 2026‑10‑17
**Firmware de referência:** v3.2 (`a6db7ef`), repo em `main` (`3128237`)

> Documento de planejamento. Prioridades já definidas pelo usuário —
> a ordem **não** é alterada aqui; só se acrescenta a subtask 1.1, que
> é dependência interna da prioridade 1.

---

## Descobertas técnicas que alteram o custo

1. **A partição OTA já existe.** O `esp32dev` usa `default.csv`, que já
   traz `app0 (ota_0)` + `app1 (ota_1)` + `otadata`. Não há o que
   reparticionar — o firmware atual (`986.480` B) cabe com folga nos
   `1.310.720` B de cada slot. A prioridade 1 é **só software**.

2. **O bug HTTP tem causa provável identificada.** `coreLoop()` faz
   `WiFi.mode(WIFI_OFF)` no meio de blecaute (economia de bateria) e
   `servidor.begin()` roda **uma única vez** em `coreIniciar()`.
   Derrubar o rádio destrói o socket de escuta do `WebServer`; quando o
   WiFi volta, `handleClient()` opera num socket morto → *connection
   refused*. Correção = re‑chamar `servidor.begin()` na transição
   `!wifiOk → wifiOk` (monitor_core.cpp, bloco de reconexão ~L979).
   Uma linha — destrava HTTP **e** OTA por HTTP.

3. **O Arduino não pode ser programado pelo ESP32 hoje — falta fio.**
   A fiação é só `Arduino TX → ESP32 GPIO16 (RX2)`. Não existe
   `ESP32 TX → Arduino RX`, nem circuito de auto‑reset. OTA do Arduino
   exige hardware novo + protocolo STK500. Confirma o tratamento
   experimental (prioridade 5).

---

## Prioridade 1 — OTA no ESP32 (Wi‑Fi, partição, rotina)

| Item | Conteúdo |
|---|---|
| **a) Objetivo / aceite** | Atualizar o firmware do ESP32 **sem desplugar** da instalação. **Aceite:** `pio run` gera `firmware.bin`; uma requisição (HTTP POST `/update` ou ArduinoOTA) grava em `app1`, o `otadata` aponta para o novo slot e o device reboota rodando a nova versão, **sem** tocar na placa. Rollback: se o novo app não sobreviver a 2 boots, volta ao anterior (mecanismo do bootloader). |
| **b) Dependências** | (i) **Corrigir o re‑listen do HTTP** (1.1); (ii) WiFi estável em algum momento (só faz OTA com rua ligada); (iii) definir transporte: **ArduinoOTA** (biblioteca padrão do framework) vs **WebServer+Update** (também padrão). |
| **c) Riscos / mitigação** | *Device vira tijolo se a gravação falhar* → `Update` com verificação de MD5 + rollback do bootloader; só reiniciar com `Update.end()` OK. *WiFi cai no meio do OTA* → abortar e manter slot antigo; nunca fazer OTA durante blecaute (gate: só com `emQueda==false && wifiOk`). *Segurança* → OTA só em rede local, com senha (`ArduinoOTA.setPassword`) e **nunca** exposto à internet. |
| **d) Esforço** | **ALTO** — mexe na flash de um device implantado e de difícil acesso; exige transporte + `Update` + salvaguardas + teste. A partição pronta derruba ~40% do risco original. |
| **e) Sequência** | 1.1 Corrigir `servidor.begin()` na reconexão (destrava HTTP). <br>1.2 Escolher transporte: **ArduinoOTA** (descobre por mDNS → resolve também o IP DHCP) **ou** `/update` via `Update`. <br>1.3 Integrar `Update.begin(WRITE_FLASH)/write()/end()` + MD5. <br>1.4 Gate de segurança: só com rua ligada e `emQueda==false`. <br>1.5 Rollback + log no Discord do slot ativo. <br>1.6 Teste em bancada antes de campo. |

> **Restrição de biblioteca atendida:** `ArduinoOTA` e `Update.h` são do
> framework arduino‑esp32 — nada de lib de terceiros.

---

## Prioridade 2 — Calibrar sensor + parâmetros MedTens

Hoje o firmware mede **amplitude pico‑a‑pico em contagens** (`amp` ~424
ligado / ~7 desligado, limiar 65) e **frequência** (já implementada,
~59,9 Hz). O MedTens (v0.1.0, projeto irmão) mede **VRMS** com
`calibration_factor=0.85`, `OFFSET_ADC=512`.

> **Metodologia importada do MedTens.** A calibração abaixo adota o
> procedimento de **9 pontos + regressão** de
> `medtens-docs/docs/calibracao.md` (repo `BlaDevHard/medtens-docs`),
> que é mais rigoroso que a heurística de 2 pontos originalmente
> planejada — dá erro residual conhecido e incerteza, exigência que
> `indice-qualidade.md` e o ADR‑0006 (DRP/DRC perante IEC/PRODIST)
> impõem para a apuração valer. Só muda o alvo: lá calibra‑se VRMS,
> aqui calibra‑se o fator `amp → vrms`.

| Item | Conteúdo |
|---|---|
| **a) Objetivo / aceite** | Converter `amp` (contagens) em **VRMS da rede em volts** e expor os parâmetros de qualidade. **Aceite:** regressão de 9 pontos com `vrms = K×amp` e **erro residual ≤ ±5%** em todos os pontos PRODIST; `K` reportado com incerteza; conferência independente do multímetro em 2–3 pontos fora da calibração dentro de ±5%. Frequência já está dentro de ±0,5 Hz. |
| **b) Dependências** | Multímetro **true‑RMS** + acesso à tomada. **Variac (autotransformador variável)** para varrer os 9 pontos PRODIST — é o que permite a calibração rigorosa; sem ele, cai no fallback de 1–2 pontos (ver **c**). O firmware atual já expõe `amp`, `min`, `max` — dá para levantar os dados de campo **sem** mudar código antes. |
| **c) Grandezas a medir e como validar** | **Metodologia de 9 pontos (importada do MedTens)** — fonte: `medtens-docs/docs/calibracao.md` (repo `BlaDevHard/medtens-docs`). <br>**Diferença chave:** o MedTens calibra um `calibration_factor` sobre **VRMS**; o monitor mede **`amp` (pico‑a‑pico em contagens)**, então um único fator **`K` (volts RMS por conta p‑p)** absorve tudo — Vref/AVcc, razão do ZMPT e a conversão p‑p→RMS (÷2√2 para senoide). Objetivo: **`vrms ≈ K × amp`**. <br>**Faixa de pontos (cobre PRODIST):** 90, 100, 110, 117, 120, 127, 133, 135, 140 V — estender a **150 V** para checar saturação. Em **cada** ponto: (1) ler `V_ref` no multímetro true‑RMS na tomada; (2) registrar `amp` (e `min`/`max`) que o Arduino reporta no mesmo instante — **N = 100 leituras**, média e desvio‑padrão; (3) `K_i = V_ref / amp_i`. <br>**Regressão:** ajustar `vrms = K × amp` por mínimos quadrados (**intercepto forçado a 0**); validar linearidade. Saídas: gráfico `amp × V_ref`, **erro residual por ponto**, e `K` com **incerteza**. <br>**Registro:** `data/calibracao_YYYY-MM-DD.csv` com `timestamp, V_ref, amp_media, sigma_amp, temperatura`; análise em `analysis/calibracao.ipynb` (mesmo padrão MedTens). <br>**Sem variac:** não dá para varrer os 9 pontos → fallback de 1–2 pontos (nominal + menor natural), registrar mesmo assim e marcar a calibração como **parcial / incerteza alta** até ter autotransformador. <br>**Cuidados:** AVcc ≈ 5 V varia (USB vs power bank) → calibrar com a alimentação **de campo (power bank)**; conferir que `min`/`max` **não** tocam 0/1023 (saturação invalida o ponto). |
| **d) Parâmetros MedTens** | **VRMS** — novo (conversão acima). **Frequência** — ✅ já existe. **Queda/interrupção** — ✅ já é o produto. **Sobretensão (swell)** — novo: limiar acima do nominal (ex. `>1.1×V_nom`). **THD** — *stretch*: exige FFT/Goertzel sobre janela de 1 ciclo a taxa maior; pesado p/ 9600 baud → **fora do escopo desta fase**. **Flicker** — o detector de pulsos da v3.2 já captura os eventos; formal IEC Pst fora de escopo. |
| **e) Esforço** | **MÉDIO** — física/medição + código moderado (VRMS + swell). Vira ALTO se entrar THD. |
| **f) Sequência** | 2.1 Levantar bancada: multímetro true‑RMS + **variac** (autotransformador) p/ varrer 90–150 V. <br>2.2 Em cada ponto PRODIST: anotar `V_ref` + `amp` (N=100, com σ) → gravar `data/calibracao_YYYY-MM-DD.csv`. <br>2.3 Regressão `vrms = K×amp` (intercepto 0) → `K` com incerteza; gráfico + erro residual. <br>2.4 Definir `K` e `V_NOM` em `config.h` (com nota da data da calibração). <br>2.5 Adicionar `vrms = K * amp` ao JSON de estado/UART. <br>2.6 Limiares de **swell** (`>1.1×V_NOM`) e **under‑voltage** (`<0.9×V_NOM`, faixa PRODIST). <br>2.7 Validar: conferir `vrms` contra o multímetro em **2–3 pontos fora da calibração** (erro ≤ ±5%). |

> ### 📌 Status da Prioridade 2 — 18/10/2026
> **Feito no modo "sem variac" (fallback ampliado para 5 pontos):**
>
> | Subtask | Estado |
> |---|---|
> | 2.1 bancada | ⚠️ **parcial** — multímetro ✅ (ANENG M118A true‑RMS), **variac ❌**. Alimentação por power bank (igual campo) ✅ |
> | 2.2 pontos | ✅ **5 pontos** em `data/calibracao_2026-10-18.csv` (N=37–58, com σ, sem saturação) — obtidos por **variação natural da rede** (123,9→130,0 V), não por variac |
> | 2.3 regressão | ✅ `K = 0,2987 ± 0,0018 (±0,60%)`, intercepto 0; maior erro residual **1,01%** (passa ≤±5%) |
> | 2.4 `K`/`V_NOM` no `config.h` | ✅ `K_VRMS 0.2976` (ponto 1, mantido — delta 0,37% p/ 0,2987, menor que incerteza) + `V_NOM 127` |
> | 2.5 `vrms` no JSON | ✅ `vrms` + `v_nom` no `/estado` **e** no MQTT. `vrmsAtual()` devolve **0** se `amp<10` (sem rua = ruído, não inventa volts) |
> | 2.6 swell/under‑voltage | ⬜ **não feito** (fica para depois, junto do `K` corrigido) |
> | 2.7 validação | ⚠️ **parcial** — erros ≤1,01% **dentro** da faixa testada; mas **span só 4,7%**, não cobre PRODIST |
>
> **Veredito:** serve para o **monitor** (volts, swell/under‑voltage);
> **NÃO** vale para apuração DRP/DRC do MedTens (exige 9 pontos PRODIST).
> O gargalo é a **faixa**, não a quantidade de pontos.
> **Registro completo:** [`calibracao-zmpt.md`](calibracao-zmpt.md).
>
> 📌 **Próximo passo anotado: SEGUNDA 19/10/2026 — medições de manhã,
> tarde e noite** (3 janelas p/ abrir o span sem variac) + 1 leitura com
> carga pesada ligada (chuveiro/forno) se possível. Depois: corrigir `K`
> para 0,2987 **na próxima gravação** (junto do OTA), nunca regravando
> só por isso.

---

## Prioridade 3 — Mensagem horária (saúde do fornecimento)

| Item | Conteúdo |
|---|---|
| **a) Objetivo / aceite** | A cada 1 h publicar um relatório de saúde, mesmo sem evento. **Aceite:** exatamente 1 msg/h no Discord/MQTT; se WiFi fora no horário, **descarta** (não polui a fila) e espera o próximo slot. |
| **b) Dependências** | Tempo confiável — ✅ (`relogio.cpp`, NTP→RTC). MQTT/Discord — ✅. Contadores de evento por hora — novos (acumuladores). |
| **c) Riscos / mitigação** | *Relatório horário cai no meio de blecaute e compete com o alerta* → suprimir enquanto `emQueda==true`; nunca misturar com QUEDA. *Fila entupindo* → relatório horário **não** entra na fila (descarta e reagendar). *Drift do relógio* → `relogioAgora()` com NTP é confiável. |
| **d) Payload mínimo** | `{"tipo":"relatorio_hora","ts":<epoch>,"hora":"<HH:MM>","energia":"presente\|ausente","vrms":<float\|omitido>,"amp":<int>,"freq_hz":<float>,"quedas_hora":<int>,"seg_sem_energia_hora":<int>,"uptime_s":<int>,"fila":<int>,"hora_conf":<bool>}` — `hora_conf=false` marca hora aproximada (sem NTP), como já faz `notaHora`. |
| **e) Esforço** | **BAIXO** — gatilho por `millis()` + acumuladores + reuso de `publicar()`/`mqttPublicarEstado()`. |
| **f) Sequência** | 3.1 Acumuladores por hora (quedas, tempo sem energia). <br>3.2 Gatilho horário com gate `!emQueda`. <br>3.3 Montar payload e enviar (Discord + MQTT). |

---

## Prioridade 4 — Confirmação pós‑oscilação

| Item | Conteúdo |
|---|---|
| **a) Objetivo / aceite** | Após relatório de oscilação/piscar, confirmar que o fornecimento está **ativo**. **Aceite:** ao fechar a janela de 5 s com estado `presente`, sai `SERVIÇO ATIVO` (verde) ligando o episódio ao retorno. |
| **b) Dependências** | Mecanismo de `SERVIÇO ATIVO` — ✅ (ciclo QUEDA→SERVIÇO). Relatório de pulsos — ✅ (`enviarRelatorioPulsos()`). |
| **c) Riscos / mitigação** | *Duplicar mensagem com a grça de 8 s / fallback* → a confirmação **não** deve sair se a grça já publicou QUEDA (`alertaAvulso`). Uma confirmação por episódio. *Confirmar num flicker que nem foi alerta* → permitir confirmação só após relatório de pulsos, não após cada pulso. |
| **d) Payload mínimo** | `{"tipo":"confirmacao","ts":<epoch>,"estado":"ativo","quedas":<n>,"flickers":<n>,"dur_ms":<total>,"desde":"<HH:MM>"}` |
| **e) Esforço** | **BAIXO** — um `publicar("SERVIÇO ATIVO", ...)` no fim de `enviarRelatorioPulsos()` com o gate de `alertaAvulso`. |
| **f) Sequência** | 4.1 Hook no fechamento da janela de pulsos. <br>4.2 Gate anti‑duplicata (`alertaAvulso`). <br>4.3 Teste com env `ciclo_teste` (`main_ciclo_teste.cpp`). |

---

## Prioridade 5 — OTA do Arduino via ESP32 — ⚠️ EXPERIMENTAL / PoC

> **Tratamento: prova de conceito de bancada, NÃO solução de produção,
> NÃO bloqueia nada.**

| Item | Conteúdo |
|---|---|
| **a) Objetivo / aceite** | Demonstrar que o ESP32 pode regravar o ATmega328P pelo UART. **Aceite de PoC (bancada):** ESP32 envia um hex trivial via **STK500v1** ao bootloader Optiboot do Uno e ele reboota com o novo firmware. **Não** é aceite de campo. |
| **b) Dependências (hardware NOVO — bloqueio real)** | Fio **ESP32 TX → Arduino RX** (não existe hoje; só há TX→RX). **Circuit o de auto‑reset** (DTR/RTS via capacitor no RESET — o chip USB‑serial do Uno faz isso; o ESP32 não). Uma UART de hardware livre no ESP32 (UART1) a 115200 (Optiboot). Espelho do `firmware.hex` + rotina STK500 no ESP32. |
| **c) Riscos / mitigação** | *Tijolo no Uno no meio de gravação* → Optiboot mantém o app antigo se a escrita não completar (troca só ao final) — PoC só em bancada. *Reset não dispara* → validar auto‑reset antes de qualquer código. *Jitter/SoftwareSerial* → usar UART de hardware, não SoftwareSerial. |
| **d) Esforço** | **ALTO** — hardware novo + protocolo STK500 do zero. Experimental justamente por ser o mais incerto. |
| **e) Sequência** | 5.1 **Wire review**: mapear pinos livres + desenhar auto‑reset. <br>5.2 Prova de reset (ESP32 pisca RESET do Uno → bootloader). <br>5.3 STK500 mínimo (`STK_GET_SYNC`/`STK_PROG_PAGE`). <br>5.4 Enviar hex mínimo. <br>5.5 **Decidir go/no‑go** antes de investir mais. |

---

## Prioridade 6 — Mi 8 (Termux + SQLite)

| Item | Conteúdo |
|---|---|
| **a) Objetivo / aceite** | Celular assina `monitor/energia/#` e grava histórico local no SQLite. **Aceite:** ao reiniciar o celular, **Termux:Boot** religa o subscriber sozinho; há histórico consultável (`sqlite3`) de ≥ 7 dias das leituras de estado. |
| **b) Dependências** | Broker (EMQX público serve para começar) — ✅. Definir **o que gravar**: estado (`presente/ausente`, `amp`/`vrms`) **e** os pulsos (hoje somem após o relatório — gravar pulsos dá o registro completo que não existe em lugar nenhum). |
| **c) Riscos / mitigação** | *Banco enche* → retenção com janela rolante (apagar > 30 dias) ou downsample de repouso. *Celular dorme/mata a conexão* → wakelock no Termux + reconexão com backoff. *Perda quando o celular morre* → aceita; ESP32 + broker continuam sendo o caminho. Mi 8 é **coletor**, não substitui o broker. |
| **d) Esforço** | **MÉDIO** — bem entendido (Python/paho + SQLite + Termux:Boot), mas setup de dispositivo + persistência de boot. |
| **e) Sequência** | 6.1 Script paho‑mqtt → SQLite (tabelas `estado`, `pulso`). <br>6.2 Retenção/limpeza. <br>6.3 Termux:Boot para auto‑start. <br>6.4 Validação de 7 dias. |

---

## Prioridade 7 — App de celular

| Item | Conteúdo |
|---|---|
| **a) Objetivo / aceite** | App que mostra estado atual + histórico de quedas. **Aceite:** abre e, em < 2 s, mostra `energia`, `vrms`/`amp` e as últimas N quedas com data/hora/duração. |
| **b) Dependências** | Fonte de dados estável: **HTTP `/estado`** (depende da correção 1.1) + **MQTT retido**; ou histórico do **Mi 8 SQLite** (prioridade 6). Enxugar o payload de campo antes de congelar o contrato de API. |
| **c) Riscos / mitigação** | *App parar quando a API mudar* → versionar o payload (`"versao":N`). *Depender de internet* → suportar modo local via Mi 8/MQTT local. *Escopo estourar* → MVP = 1 tela de estado + 1 de histórico. |
| **d) Esforço** | **ALTO** — cliente completo + camada de dados + UI. |
| **e) Sequência** | 7.1 Congelar contrato de dados (P2/P3 já definem os campos). <br>7.2 MVP 1 tela (estado). <br>7.3 Histórico. <br>7.4 Notificação local de queda. |

---

## Roadmap em 4 fases

```
FASE 1 — OTA + calibração  (prioridades 1 + 2)
  1.1 corrigir re-listen do HTTP (destrava HTTP + OTA-HTTP)
  1.2..1.6 OTA via ArduinoOTA/Update, com gate de segurança e rollback
  2.1..2.7 calibração VRMS — 9 pontos PRODIST + regressão (variac); swell/under-voltage; THD fora do escopo
  → Entrega: atualização remota + tensão em volts confiável

FASE 2 — mensagens periódicas e pós-evento  (prioridades 3 + 4)
  3.1..3.3 relatório horário (payload mínimo definido, suprimido em queda)
  4.1..4.3 confirmação SERVIÇO ATIVO pós-oscilação (sem duplicata)
  → Entrega: visão contínua + fechamento de episódio

FASE 3 — PoC OTA do Arduino  (prioridade 5)  [EXPERIMENTAL]
  5.1..5.5 wire review → auto-reset → STK500 → hex → decisão go/no-go
  → Entrega: prova de bancada; NÃO bloqueia as fases 1/2/4

FASE 4 — Mi 8 + app  (prioridades 6 + 7)
  6.1..6.4 coletor Termux/SQLite com Termux:Boot
  7.1..7.4 MVP do app sobre o contrato congelado
  → Entrega: histórico persistente + interface
```

**Justificativa de manter a ordem:** a Fase 1 vem primeiro porque a
correção 1.1 destrava o HTTP que o OTA (e depois o app) precisa, e a
calibração dá os volts que os relatórios das Fases 2 e 7 publicam — cada
fase consome a anterior. A única inserção é a **subtask 1.1**, e ela é
dependência interna da prioridade 1, não reordenação.
