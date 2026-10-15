/*
 * GATEWAY UART (ATIVO na v3) — ESP32 substitui o ESP-01
 * ====================================================
 * Le o Arduino pela UART2 e faz todo o resto: Discord +
 * Pushbullet, RTC interno + NTP, MQTT, servidor HTTP, fila de
 * mensagens (rede fora) e checkpoint na flash.
 *
 * PROTOCOLO (9600 8N1, uma linha JSON por ciclo de ~500 ms):
 *
 *   {"boot":"Sensor OK","versao":"3.1"}
 *   {"energia":"presente","amp":424}
 *   {"energia":"ausente","amp":7}
 *
 * - "energia" e o contrato da v2 e continua sendo o que move a
 *   maquina de estados (presente = rua alimentada).
 * - "amp" e a amplitude pico-a-pico do ZMPT no A0 do Arduino.
 *   Com rua ~424, sem rua ~7, limiar 65.
 * - O Arduino tambem manda linhas de diagnostico (comecam com "#"),
 *   que sao ignoradas aqui.
 *
 * (A v3.0 enviava "fonte"/"vcc" — deteccao pela propria
 * alimentacao do Arduino. Removido na v3.1.)
 *
 * CONEXOES:
 *   Arduino TX (pino 1) -> divisor 1k/2k -> ESP32 GPIO16 (RX2)
 *   Arduino GND         -> ESP32 GND
 *
 * A gravacao NAO exige desligar fio nenhum: a UART2 (GPIO16)
 * e separada da UART0 do USB.
 */

#include "monitor_core.h"
#include "config.h"

void setup() {
  coreIniciar("gateway_uart");

  // UART2: so RX (TX fica desconectado). 9600 8N1 = protocolo do Arduino.
  // Pull-up interno ANTES do begin. NUNCA chamar pinMode() DEPOIS do
  // Serial2.begin — isso desconecta o pino da matriz da UART2 e a
  // recepcao morre (digitalRead continua funcionando, a UART nao).
  pinMode(PINO_RX_ARDUINO, INPUT_PULLUP);
  Serial2.begin(9600, SERIAL_8N1, PINO_RX_ARDUINO, -1);
  Serial2.setTimeout(200);

  Serial.println("UART2 pronta: RX=GPIO" + String(PINO_RX_ARDUINO) +
                 " @9600, aguardando linhas do Arduino");
}

void loop() {
  coreLoop();

  // Le apenas linhas JSON completas; linhas de boot e de
  // diagnostico (VCC=...) nao comecam com "{" e caem aqui.
  while (Serial2.available() > 0) {
    String linha = Serial2.readStringUntil('\n');
    linha.trim();
    if (!linha.startsWith("{")) continue;

    sensorPulsou();

    String energia = extrairTexto(linha, "\"energia\":\"");
    if (energia.length() == 0) continue;   // ex.: {"boot":"Sensor OK",...}

    int amp = extrairInteiro(linha, "\"amp\":");   // amplitude do ZMPT

    processarEstado(energia, amp);
  }
}
