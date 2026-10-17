/*
 * MONITOR DE QUEDAS DE ENERGIA — GATEWAY ESP-01
 * =================================================
 * Recebe dados do Arduino via Serial (9600 baud)
 * Envia alertas para:
 *   1. Pushbullet  -> notificacao no celular (instantaneo)
 *   2. Discord     -> log auditavel com timestamp
 *
 * CONEXOES:
 *   Arduino TX (pino 1) -> divisor 1k/2k -> ESP-01 RX (GPIO3)
 *   Arduino GND         -> ESP-01 GND
 *   ESP-01 VCC e CH_PD  -> 3.3V (NUNCA 5V!)
 *
 * ESTADOS (o Arduino ja envia pronto, sem calculo de tensao):
 *   energia "presente" -> SERVICO ATIVO
 *   energia "ausente"  -> QUEDA
 *   (1a leitura apos o boot publica o estado atual)
 *
 * GRAVACAO: GPIO0 no GND + reconectar USB
 * RODANDO:  tirar GPIO0 do GND + reconectar USB
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "config.h"   // WiFi, tokens e identificacao (nao versionado)

// Fuso horario Brasil (GMT-3)
const long GMT_OFFSET = -3 * 3600;

// Sem limiares aqui: o Arduino ja envia se a energia
// esta presente ou ausente (deteccao de presenca puro)

// ==================================================================

WiFiClientSecure clienteSeguro;
HTTPClient http;

bool emQueda = false;
unsigned long tsQueda = 0;
bool wifiOk = false;
bool estadoInicialEnviado = false;   // estado atual ja publicado no boot

void setup() {
  Serial.begin(9600);           // mesma velocidade do Arduino
  delay(100);

  pinMode(2, OUTPUT);           // LED onboard (GPIO2, ativo em LOW)
  digitalWrite(2, HIGH);        // LED apagado

  conectarWiFi();

  if (wifiOk) {
    configTime(GMT_OFFSET, 0, "pool.ntp.org", "time.google.com");
    delay(1500);
    clienteSeguro.setInsecure();  // nao valida certificado (economiza RAM)

    // Post inicial: prova de vida
    String msg = "Monitor de quedas iniciado\n" + identificacao() + agoraStr();
    enviarPushbullet("SISTEMA ONLINE", msg);
    enviarDiscord("SISTEMA ONLINE", msg, 0x00FFFF);  // ciano
  }
}

void loop() {
  // --- Reconecta WiFi se caiu ---
  if (WiFi.status() != WL_CONNECTED) {
    wifiOk = false;
    conectarWiFi();
    if (wifiOk) {
      clienteSeguro.setInsecure();
    }
    return;
  }
  wifiOk = true;

  // --- Le dados do Arduino ---
  if (Serial.available() > 0) {
    String linha = Serial.readStringUntil('\n');
    linha.trim();
    if (linha.startsWith("{")) {
      processar(linha);
    }
  }

  // --- LED de batimento (pisca a cada 2s) ---
  static unsigned long ultimoPisca = 0;
  if (millis() - ultimoPisca > 2000) {
    digitalWrite(2, !digitalRead(2));
    ultimoPisca = millis();
  }
}

// ==================================================================
//  LOGICA DE QUEDA / SERVICO ATIVO (presenca, sem tensao)
// ==================================================================
void processar(String json) {
  String estado = extrairEstado(json);
  if (estado.length() == 0) return;   // nao e leitura de energia, ignora

  Serial.print("ESTADO=");
  Serial.println(estado);   // debug (visivel no monitor serial)

  // --- 1a leitura apos o boot: publica o estado atual ---
  // (importante: se o no-break acabar, ao voltar o boot
  //  envia imediatamente se esta em QUEDA ou SERVICO ATIVO)
  if (!estadoInicialEnviado) {
    estadoInicialEnviado = true;
    emQueda = (estado == "ausente");
    if (emQueda) tsQueda = millis();

    String titulo = emQueda ? String("QUEDA") : String("SERVIÇO ATIVO");
    String msg = "Estado ao iniciar o sistema\n";
    msg += "Situação: " + titulo + "\n";
    msg += identificacao();
    msg += agoraStr();

    Serial.println("ESTADO INICIAL: " + titulo);
    enviarPushbullet(titulo, msg);
    enviarDiscord(titulo, msg, emQueda ? 0xFF0000 : 0x00FF00);
    return;
  }

  // --- Sem energia -> QUEDA ---
  if (estado == "ausente" && !emQueda) {
    emQueda = true;
    tsQueda = millis();

    String msg = "Sem serviço COPEL\n";
    msg += identificacao();
    msg += agoraStr();

    Serial.println("EVENTO: QUEDA");
    enviarPushbullet("QUEDA", msg);
    enviarDiscord("QUEDA", msg, 0xFF0000);  // vermelho
  }

  // --- Energia de volta -> SERVICO ATIVO ---
  else if (estado == "presente" && emQueda) {
    emQueda = false;
    unsigned long dur = millis() - tsQueda;

    String msg = "Energia restabelecida\n";
    msg += "Duração da queda: " + formatarDuracao(dur) + "\n";
    msg += identificacao();
    msg += agoraStr();

    Serial.println("EVENTO: SERVICO ATIVO");
    enviarPushbullet("SERVIÇO ATIVO", msg);
    enviarDiscord("SERVIÇO ATIVO", msg, 0x00FF00);  // verde
  }
}

// ==================================================================
//  PUSHBULLET (notificacao no celular)
// ==================================================================
void enviarPushbullet(String titulo, String corpo) {
  if (!wifiOk) return;

  http.begin(clienteSeguro, "https://api.pushbullet.com/v2/pushes");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Access-Token", PB_TOKEN);

  String payload = "{\"type\":\"note\",\"title\":\"" + escapar(titulo) +
                   "\",\"body\":\"" + escapar(corpo) + "\"}";

  int codigo = http.POST(payload);
  Serial.println("Pushbullet: " + String(codigo));

  http.end();
}

// ==================================================================
//  DISCORD (log auditavel)
// ==================================================================
void enviarDiscord(String titulo, String descricao, int cor) {
  if (!wifiOk) return;

  http.begin(clienteSeguro, DISCORD_URL);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"embeds\":[{\"title\":\"" + escapar(titulo) +
                   "\",\"description\":\"" + escapar(descricao) +
                   "\",\"color\":" + String(cor) +
                   ",\"footer\":{\"text\":\"Monitor de Quedas - Antonina-PR\"}}]}";

  int codigo = http.POST(payload);
  Serial.println("Discord: " + String(codigo));   // 204 = ok

  http.end();
}

// ==================================================================
//  UTILITARIOS
// ==================================================================
// Identificacao do imovel (aparece em todas as mensagens)
String identificacao() {
  return String(LOCAL_TXT) + "\n" + CEP_TXT + "\n" + UC_TXT + "\n";
}

void conectarWiFi() {
  Serial.print("Conectando WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) {
    delay(500);
    Serial.print(".");
    t++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    Serial.println(" OK -> " + WiFi.localIP().toString());
  } else {
    wifiOk = false;
    Serial.println(" FALHOU (verifique SSID/senha)");
  }
}

// Extrai o campo "energia":"presente|ausente" do JSON.
// Retorna "" se nao encontrar (ex: linha de boot do Arduino)
String extrairEstado(String json) {
  String chave = "\"energia\":\"";
  int i = json.indexOf(chave);
  if (i < 0) return "";
  i += chave.length();
  int f = json.indexOf('"', i);
  if (f < 0) return "";
  return json.substring(i, f);
}

String escapar(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  return s;
}

String agoraStr() {
  time_t agora = time(nullptr);
  struct tm* info = localtime(&agora);
  char buf[40];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", info);
  return String("Hora: ") + buf;
}

String formatarDuracao(unsigned long ms) {
  unsigned long seg = ms / 1000;
  unsigned long min = seg / 60;
  unsigned long hora = min / 60;
  seg %= 60;
  min %= 60;

  if (hora > 0) return String(hora) + "h " + String(min) + "m " + String(seg) + "s";
  if (min > 0) return String(min) + "m " + String(seg) + "s";
  return String(seg) + "s";
}
