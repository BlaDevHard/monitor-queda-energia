/*
 * CONFIGURACAO — EXEMPLO (ESP32)
 * ==============================
 * Copie este arquivo para `config.h` e preencha com seus dados:
 *
 *     cp config.example.h config.h
 *
 * O arquivo `config.h` real esta no .gitignore e NUNCA deve
 * ser enviado ao GitHub.
 *
 * NOTA: os ponteiros sao `const ... const` de proposito — os dois
 * firmwares incluem este arquivo e sem o `const` extra daria erro
 * de simbolo duplicado na hora de linkar.
 */

// ---- WiFi (rede 2.4GHz) ----
const char* const WIFI_SSID = "SEU_SSID_AQUI";
const char* const WIFI_PASS = "SUA_SENHA_AQUI";

// ---- Pushbullet (notificacao no celular) ----
// https://www.pushbullet.com/#settings/account  -> "Gerar novo acesso"
const char* const PB_TOKEN = "SEU_TOKEN_PUSHBULLET_AQUI";

// ---- Discord Webhook (log auditavel com timestamp) ----
// Discord > Canal > Editar canal > Integracoes > Webhooks > Novo webhook
const char* const DISCORD_URL =
  "https://discord.com/api/webhooks/SEU_WEBHOOK_AQUI";

// ---- Identificacao do imovel (aparece em todas as mensagens) ----
const char* const LOCAL_TXT = "Local: SEU_MUNICIPIO";
const char* const CEP_TXT   = "CEP: 00000000";
const char* const UC_TXT    = "UNIDADE CONSUMIDORA: 000000000000000";

// ============================================================
// Documentacao do projeto
// ============================================================
// URL publica do repositorio. Enviada na mensagem SISTEMA ONLINE
// e republicada sozinha a cada DOCS_REPUBLICA_MS. Motivo: no
// Discord a conversa rola, o link do boot sai de vista em poucos
// dias, e o pior momento para procurar a documentacao e quando a
// rua cai.
const char* const DOCS_URL = "https://github.com/SEU_USUARIO/SEU_REPO";
#define DOCS_REPUBLICA_MS 604800000UL   // 7 dias (604800000 ms)

// ============================================================
// MQTT — base para o futuro app Android
// ============================================================
// O estado e publicado RETIDO (retained) em MQTT_TOPICO_ESTADO:
// ao assinar o topico, o app recebe imediatamente o estado atual,
// mesmo sem ter visto a transicao. MQTT_TOPICO_STATUS leva o
// LWT (last will): "online" enquanto o ESP estiver vivo,
// "offline" quando cair.
//
// Broker publico abaixo e so para teste. Para producao, aponte
// para um broker proprio (Mosquitto) ou pago.
#define MQTT_ATIVO 1
const char* const MQTT_BROKER = "broker.emqx.io";
const int          MQTT_PORTA = 1883;
const char* const MQTT_TOPICO_ESTADO = "monitor/energia/estado";
const char* const MQTT_TOPICO_STATUS = "monitor/energia/status";

// ============================================================
// Modo mudo — bancada
// ============================================================
// 1 = simula sem notificar ninguem: nada sai para Discord,
// Pushbullet ou MQTT. A mensagem que SERIA enviada aparece no
// monitor serial, marcada "[MODO MUDO]". Use para validar a
// cadeia toda sem gastar o webhook.
// 0 = producao.
#define MODO_MUDO 0

// ============================================================
// Calibracao do ZMPT — conversao amp -> VRMS
// ============================================================
// vrms = K_VRMS * amp   (amp = amplitude pico-a-pico em contagens)
//
// K_VRMS = V_ref / amp_medido, medido com multímetro true-RMS nos
// terminais de ENTRADA do ZMPT.
//
// STATUS 18/10/2026: **CALIBRACAO PARCIAL MELHORADA — 5 pontos,
// faixa estreita, sem variac.** Regressao (intercepto 0):
//     K = 0.2987 +- 0.0018 (0,60%)   maior erro residual 1,01%
// Pontos: 123.9-130.0 V (span 4,7%), obtidos por variacao natural
// da rede, nao por variac. Nenhum ponto saturado.
// Detalhe, dados e plano: docs/calibracao-zmpt.md
//
// POR QUE 0.2976 AQUI e nao 0.2987? O firmware foi gravado com o
// valor do ponto 1; o delta para 0.2987 e de 0,37% — MENOR que a
// incerteza do proprio multimetro (~0,5-0,7%). Decisao 18/10:
// manter e corrigir na PROXIMA gravacao ( junto do OTA), porque
// regravar agora so por isso reiniciaria a validacao de campo da
// v3.3 sem ganho mensuravel.
//
// ATENCAO: valida p/ o MONITOR (volts, swell/under-voltage), NAO p/
// apuracao DRP/DRC do MedTens — isso exige os 9 pontos PRODIST
// (90-150 V) com variac.
//
// V_NOM: nominal da rede monitorada (PRODIST módulo 8: 127 V).
#define K_VRMS 0.2976   // volts RMS por conta pico-a-pico
#define V_NOM  127      // tensao nominal da rede (V)

// ============================================================
// Relatorio horario de saude do fornecimento
// ============================================================
// A cada HORARIA_MS de ULTIMO ENVIO, uma mensagem "SERVIÇO
// PERMANECE ATIVO" sai no Discord. Contado do envio, nao do
// relogio: no blecaute nao ha WiFi; quando a rede volta o
// relatorio sai sozinho (apos a fila de alertas) — vale como
// confirmacao de retorno. Suprimido durante queda em andamento.
#define HORARIA_MS 3600000   // 60 min em ms

// ============================================================
// v3.1 — tempo, fila de mensagens e checkpoint
// ============================================================
// O roteador cai junto com a rua: no instante da QUEDA nao ha
// internet. As mensagens ficam na fila ate o WiFi voltar.
// Se a fila encher, a MAIS ANTIGA e descartada (a queda em
// andamento importa mais que a antiga).
#define FILA_MAX 32   // ~4 KB de RAM (cabe folgado nos 320 KB do ESP32)

// Periodicidade do checkpoint na flash (epoch + estado).
// Ele e o que permite reconstruir a queda se o power bank
// esgotar e tudo desligar.
#define CHECKPOINT_MS 60000

// Watchdog: se o Arduino parar de falar, alerta no Discord.
#define AVISAR_SEM_SENSOR 1
#define SENSOR_SILENCIO_MS 45000   // 45 s de silencio

// ============================================================
// gateway_uart (ATIVO)
// ============================================================
// RX2 do ESP32. So mude se estiver usando o pino para outra coisa.
// (Na gravacao NAO precisa desligar o fio do Arduino: a UART2
//  e diferente da UART0 usada pelo USB.)
#define PINO_RX_ARDUINO 16

// ============================================================
// Opcao 2 — unificado_adc (LEGADO)
// ============================================================
// Alternativa: um chip so, com o ZMPT101B direto no ADC do
// ESP32. Nao esta em uso — o gateway_uart (Arduino + ESP32) e o
// caminho ativo. Mantido como alternativa.
//
// Tem que ser pino ADC1 (GPIO 32..39): com WiFi ligado o ADC2 do
// ESP32 fica indisponivel.
#define PINO_ZMPT 34

// Limiar de presenca em contagens do ADC do ESP32 (12 bits, 3.3 V).
// PROVISORIO — o caminho ativo mede no Arduino (10 bits, 5 V, limiar 65).
// Ver esp32/README.md para ajustar observando o monitor serial
// (NAO precisa de multimedro).
#define LIMITE_AMPLITUDE 400

// Leituras identicas seguidas para confirmar mudanca de estado.
// Com CICLO_MS=665, 3 confirmacoes = ~2 s (igual ao Arduino).
#define CONFIRMACOES 3

// Janela de amostragem do ADC: ~166 ms = ~10 ciclos de 60 Hz
#define JANELA_MS 166

// Ritmo de leitura: mesmo do Arduino (165 ms de janela + 500 ms)
#define CICLO_MS 665

// Vigilancia de saturacao: quantas janelas seguidas tocando as
// bordas do ADC (0/4095) antes de alertar "ADC SATURADO".
// (3 janelas ~2 s — mesmo tempo do debounce)
#define JANELAS_ALERTA 3
