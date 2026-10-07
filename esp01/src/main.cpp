#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <time.h>

// ==================== CONFIGURACOES ====================

const char* WIFI_SSID = "SEU_SSID_AQUI";
const char* WIFI_PASS = "SUA_SENHA_AQUI";

const char* IFTTT_HOST = "https://maker.ifttt.com";
const char* IFTTT_KEY = "SUA_CHAVE_IFTTT";
const char* IFTTT_EVENTO = "queda_energia";

// Brasilia: UTC-3.
const long GMT_OFFSET_SECONDS = -3L * 3600L;
const unsigned long WIFI_TIMEOUT_MS = 30000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
const unsigned long NTP_RETRY_INTERVAL_MS = 60000;

WiFiClient wifiClient;
ESP8266WebServer servidor(80);
unsigned long ultimoWifiRetry = 0;
unsigned long ultimoNtpRetry = 0;
bool mdnsAtivo = false;

bool conectaWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.print(F("Conectando ao WiFi"));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Falha ao conectar ao WiFi."));
    return false;
  }

  Serial.print(F("WiFi conectado. IP: "));
  Serial.println(WiFi.localIP());

  if (MDNS.begin("monitor-energia")) {
    mdnsAtivo = true;
    MDNS.addService("http", "tcp", 80);
    Serial.println(F("Acesso local: http://monitor-energia.local/"));
  } else {
    mdnsAtivo = false;
    Serial.println(F("mDNS indisponível; use o endereco IP acima."));
  }

  return true;
}

String paginaStatus() {
  const bool conectado = WiFi.status() == WL_CONNECTED;
  const time_t agora = time(nullptr);
  String html;
  html.reserve(1800);
  html += F("<!doctype html><html lang='pt-BR'><head>");
  html += F("<meta charset='utf-8'><meta name='viewport' "
            "content='width=device-width,initial-scale=1'>");
  html += F("<title>Monitor de Energia Gateway</title>");
  html += F("<style>body{font-family:Arial;max-width:680px;margin:32px auto;"
            "padding:0 16px;color:#222}section{border:1px solid #ddd;"
            "border-radius:8px;padding:16px;margin:12px 0}code{background:#f3f3f3;"
            "padding:3px 6px;border-radius:4px}.ok{color:#087f23}</style></head><body>");
  html += F("<h1>Monitor de Energia Gateway ESP-01</h1><section>");
  html += F("<p>Estado Wi-Fi: <strong class='ok'>");
  html += conectado ? F("conectado") : F("desconectado");
  html += F("</strong></p><p>IP: <code>");
  html += conectado ? WiFi.localIP().toString() : F("-");
  html += F("</code></p><p>Hostname: <code>monitor-energia.local</code></p>");
  html += F("<p>Uptime: <code>");
  html += String(millis() / 1000);
  html += F(" s</code></p><p>Horário NTP: <code>");
  if (agora >= 1600000000) {
    struct tm* informacao = localtime(&agora);
    char dataHora[25];
    strftime(dataHora, sizeof(dataHora), "%d/%m/%Y %H:%M:%S", informacao);
    html += dataHora;
  } else {
    html += F("não sincronizado");
  }
  html += F("</code></p></section><p><a href='/status'>Ver status JSON</a></p>");
  html += F("</body></html>");
  return html;
}

void configuraServidorWeb() {
  servidor.on("/", HTTP_GET, []() {
    servidor.send(200, F("text/html; charset=utf-8"), paginaStatus());
  });

  servidor.on("/status", HTTP_GET, []() {
    String json = F("{\"wifi\":\"");
    json += WiFi.status() == WL_CONNECTED ? F("conectado") : F("desconectado");
    json += F("\",\"ip\":\"");
    json += WiFi.localIP().toString();
    json += F("\",\"hostname\":\"monitor-energia.local\",\"uptime_s\":");
    json += String(millis() / 1000);
    json += F("}");
    servidor.send(200, F("application/json; charset=utf-8"), json);
  });

  servidor.on("/health", HTTP_GET, []() {
    servidor.send(200, F("text/plain; charset=utf-8"), F("OK"));
  });

  servidor.onNotFound([]() {
    servidor.send(404, F("text/plain; charset=utf-8"), F("Rota nao encontrada"));
  });

  servidor.begin();
  Serial.println(F("Servidor HTTP local iniciado na porta 80."));
}

void atualizaNtp() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long agoraMs = millis();
  if (ultimoNtpRetry != 0 &&
      agoraMs - ultimoNtpRetry < NTP_RETRY_INTERVAL_MS) {
    return;
  }

  ultimoNtpRetry = agoraMs;
  configTime(GMT_OFFSET_SECONDS, 0, "pool.ntp.org", "time.nist.gov");

  const time_t inicio = time(nullptr);
  const unsigned long inicioMs = millis();
  while (time(nullptr) < 1600000000 &&
         millis() - inicioMs < 5000) {
    delay(100);
  }

  if (time(nullptr) >= 1600000000) {
    Serial.println(F("NTP sincronizado."));
  } else if (inicio < 1600000000) {
    Serial.println(F("NTP ainda não sincronizado."));
  }
}

uint32_t extraiNumero(const String& texto, const char* chave) {
  int inicio = texto.indexOf(chave);
  if (inicio < 0) {
    return 0;
  }

  inicio += strlen(chave);
  int fim = texto.indexOf(',', inicio);
  if (fim < 0) {
    fim = texto.indexOf('}', inicio);
  }
  if (fim < 0) {
    return 0;
  }

  return texto.substring(inicio, fim).toInt();
}

float extraiDecimal(const String& texto, const char* chave) {
  int inicio = texto.indexOf(chave);
  if (inicio < 0) {
    return 0.0f;
  }

  inicio += strlen(chave);
  int fim = texto.indexOf(',', inicio);
  if (fim < 0) {
    fim = texto.indexOf('}', inicio);
  }
  if (fim < 0) {
    return 0.0f;
  }

  return texto.substring(inicio, fim).toFloat();
}

bool enviaParaIFTTT(uint32_t tsRelativo, float vrms, uint32_t duracaoMs) {
  (void)tsRelativo;

  if (!conectaWiFi()) {
    return false;
  }

  atualizaNtp();

  time_t agora = time(nullptr);
  struct tm* informacao = localtime(&agora);
  char dataHora[25] = "data indisponivel";
  if (informacao != nullptr && agora >= 1600000000) {
    strftime(dataHora, sizeof(dataHora), "%d/%m/%Y %H:%M:%S", informacao);
  }

  const String valor1 = String(dataHora);
  const String valor2 = String(vrms, 1) + F(" V");
  const String valor3 = duracaoMs > 0
                            ? String(duracaoMs) + F(" ms")
                            : String(F("INICIO"));

  const String payload = String(F("{\"value1\":\"")) + valor1 +
                         F("\",\"value2\":\"") + valor2 +
                         F("\",\"value3\":\"") + valor3 + F("\"}");
  const String url = String(IFTTT_HOST) + F("/trigger/") + IFTTT_EVENTO +
                     F("/with/key/") + IFTTT_KEY;

  HTTPClient http;
  if (!http.begin(wifiClient, url)) {
    Serial.println(F("Não foi possível iniciar requisicao HTTP."));
    return false;
  }

  http.addHeader(F("Content-Type"), F("application/json"));
  Serial.println(F("Enviando evento para IFTTT..."));
  const int resposta = http.POST(payload);
  http.end();

  if (resposta == HTTP_CODE_OK) {
    Serial.println(F("Evento enviado com sucesso."));
    return true;
  }

  Serial.print(F("Erro HTTP: "));
  Serial.println(resposta);
  return false;
}

void processaLinha(String linha) {
  linha.trim();
  if (linha.length() == 0) {
    return;
  }

  if (linha.indexOf(F("\"evt\":\"QUEDA\"")) >= 0) {
    const uint32_t timestamp = extraiNumero(linha, "\"ts\":");
    const float tensaoRms = extraiDecimal(linha, "\"vrms\":");
    const uint32_t duracao = extraiNumero(linha, "\"dur_ms\":");
    enviaParaIFTTT(timestamp, tensaoRms, duracao);
    return;
  }

  if (linha.indexOf(F("\"vrms\":")) >= 0 &&
      extraiDecimal(linha, "\"vrms\":") < 0.1f) {
    Serial.println(F("Arduino: tensao muito baixa."));
  }
}

void setup() {
  Serial.begin(9600);
  Serial.setTimeout(1000);
  delay(100);

  Serial.println();
  Serial.println(F("MONITOR ENERGIA Gateway ESP-01 iniciando..."));
  conectaWiFi();
  atualizaNtp();
  configuraServidorWeb();
  Serial.println(F("Pronto. Aguardando Arduino..."));
}

void loop() {
  servidor.handleClient();
  if (mdnsAtivo) {
    MDNS.update();
  }

  if (Serial.available()) {
    processaLinha(Serial.readStringUntil('\n'));
  }

  if (WiFi.status() != WL_CONNECTED &&
      millis() - ultimoWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
    ultimoWifiRetry = millis();
    conectaWiFi();
  }
}
