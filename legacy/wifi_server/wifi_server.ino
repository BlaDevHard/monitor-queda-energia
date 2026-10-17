/*
 * SERVIDOR WIFI - TESTE ESP-01
 * Conecta na sua rede e abre uma pagina no navegador
 *
 * ACESSO: http://<ip-do-esp>  (ip aparece no monitor serial e na rede)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

#include "config.h"   // WiFi (copie config.example.h para config.h)

ESP8266WebServer server(80);

String ultimaLinhaArduino = "(nenhum dado recebido do Arduino)";
unsigned long bootMs = 0;

void setup() {
  Serial.begin(9600);
  delay(100);

  pinMode(2, OUTPUT);       // LED onboard
  digitalWrite(2, HIGH);    // apagado (ativo baixo)

  bootMs = millis();

  // Conecta no WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 60) {
    delay(500);
    digitalWrite(2, !digitalRead(2));  // pisca enquanto conecta
    t++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(2, LOW);   // LED fixo = conectado
    Serial.println("WIFI OK");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("FALHA NO WIFI (verifique SSID/senha/rede 2.4GHz)");
  }

  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  // Le dados vindos do Arduino (Serial)
  if (Serial.available() > 0) {
    String l = Serial.readStringUntil('\n');
    l.trim();
    if (l.length() > 0) {
      ultimaLinhaArduino = l;
    }
  }

  server.handleClient();
}

void handleRoot() {
  String html = "<!DOCTYPE html><html lang='pt-BR'><head>"
                "<meta charset='UTF-8'>"
                "<meta http-equiv='refresh' content='5'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>ESP-01 - Monitor de Quedas</title>"
                "<style>"
                "body{font-family:monospace;background:#1a1a2e;color:#eee;padding:20px}"
                ".c{background:#16213e;border:2px solid #0f3460;padding:16px;"
                "margin:10px 0;border-radius:8px}"
                ".k{color:#4ecca3}.v{color:#f39c12;font-size:1.2em}"
                "</style></head><body>";

  html += "<h1>ESP-01 ONLINE</h1>";

  html += "<div class='c'><span class='k'>Rede: </span>"
          + String(WiFi.SSID()) + "<br>"
          + "<span class='k'>Sinal (RSSI): </span>"
          + String(WiFi.RSSI()) + " dBm<br>"
          + "<span class='k'>IP: </span>"
          + WiFi.localIP().toString() + "<br>"
          + "<span class='k'>MAC: </span>"
          + WiFi.macAddress() + "</div>";

  html += "<div class='c'><span class='k'>No ar ha: </span>"
          + formatarUptime(millis() - bootMs) + "<br>"
          + "<span class='k'>Memoria livre: </span>"
          + String(ESP.getFreeHeap()) + " bytes<br>"
          + "<span class='k'>Build: </span>03/10/2026</div>";

  html += String("<div class='c'><span class='k'>Ultimo dado do Arduino:</span><br>")
          + "<span class='v'>" + ultimaLinhaArduino + "</span></div>";

  html += "<p style='opacity:.6'>Atualiza automaticamente a cada 5s</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

String formatarUptime(unsigned long ms) {
  unsigned long s = ms / 1000;
  unsigned long m = s / 60;
  unsigned long h = m / 60;
  s %= 60; m %= 60;
  if (h > 0) return String(h) + "h " + String(m) + "m " + String(s) + "s";
  if (m > 0) return String(m) + "m " + String(s) + "s";
  return String(s) + "s";
}
