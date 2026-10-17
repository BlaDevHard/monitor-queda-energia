/*
 * CONFIGURACAO — EXEMPLO
 * =======================
 * Copie este arquivo para `config.h` e preencha com seus dados:
 *
 *     cp config.example.h config.h
 *
 * O arquivo `config.h` real esta no .gitignore e NUNCA deve
 * ser enviado ao GitHub.
 */

// ---- WiFi (rede 2.4GHz — o ESP-01 NAO ve redes 5GHz) ----
const char* WIFI_SSID = "SEU_SSID_AQUI";
const char* WIFI_PASS = "SUA_SENHA_AQUI";

// ---- Pushbullet (notificacao no celular) ----
// https://www.pushbullet.com/#settings/account  -> "Gerar novo acesso"
const char* PB_TOKEN = "SEU_TOKEN_PUSHBULLET_AQUI";

// ---- Discord Webhook (log auditavel com timestamp) ----
// Discord > Canal > Editar canal > Integracoes > Webhooks > Novo webhook
const char* DISCORD_URL =
  "https://discord.com/api/webhooks/SEU_WEBHOOK_AQUI";

// ---- Identificacao do imovel (aparece em todas as mensagens) ----
const char* LOCAL_TXT = "Local: SEU_MUNICIPIO";
const char* CEP_TXT   = "CEP: 00000000";
const char* UC_TXT    = "UNIDADE CONSUMIDORA: 000000000000000";
