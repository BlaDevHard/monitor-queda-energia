#pragma once
#include <Arduino.h>

/*
 * NUCLEO COMPARTILHADO — Monitor de Quedas de Energia (ESP32)
 * ===========================================================
 * v3.1: o Arduino mede a RUA direto, pelo ZMPT101B no A0 — o
 * sinal alternada da rede, por amplitude pico-a-pico. Quem
 * carimba a hora e publica e este nucleo.
 *
 * (Na v3.0 o Arduino media a propria Vcc e inferia a queda pela
 * troca tomada -> power bank. Aquilo exigia calibracao de limiar
 * e nao media a rua; foi removido, junto com "fonte"/"vcc".)
 *
 * Firmwares que usam este nucleo:
 *
 *   - gateway_uart  (ATIVO): le o Arduino pela UART2
 *                     Arduino TX -> divisor 1k/2k -> GPIO16 (RX2)
 *
 *   - unificado_adc (legado): le o ZMPT direto no ADC do ESP32.
 *                     Alternativa se um chip so for suficiente.
 *
 * O nucleo faz: WiFi + NTP/RTC + Discord + Pushbullet
 *               + MQTT retido + servidor HTTP (/ , /estado)
 *               + FILA de mensagens (o roteador cai junto com a
 *                 rua, entao no momento da queda nao ha internet)
 *               + CHECKPOINT na flash (se o power bank esgotar e
 *                 tudo desligar, ao voltar sabemos da queda)
 *
 * O nucleo NAO le sensor: os firmwares chamam processarEstado()
 * quando tem um estado. O nucleo so REAGE a transicoes.
 */

// ---- estado global (compartilhado com os firmwares) ----
extern bool emQueda;
extern bool estadoInicialEnviado;
extern String estadoAtual;   // "presente" | "ausente" | "?"
extern int ampAtual;         // amplitude do ZMPT (-1 = sem dado)
extern bool wifiOk;
extern bool sinalSaturado;   // true = ADC fora da faixa (firmware ADC)

// ---- ciclo de vida ----
void coreIniciar(const char* nomeFirmware);  // WiFi, NTP/RTC, HTTP, MQTT
void coreLoop();                             // reconexao, HTTP, MQTT, fila, LED

// ---- chamado pelos firmwares a cada leitura confirmada ----
// energia: "presente" | "ausente"  (contrato da v2, mantido)
// amp:     amplitude pico-a-pico do ZMPT (-1 se desconhecido)
void processarEstado(const String& energia, int amp);

// ---- alerta de hardware (ex.: ADC saturado) ----
// Envia para Discord + Pushbullet com identificacao e hora.
// Se nao houver rede, fica na fila e sai no reconnect.
void alertaHardware(const String& titulo, const String& corpo);

// ---- batimento do sensor ----
// Chamar sempre que chegar um dado valido do Arduino. Alimenta o
// watchdog "SENSOR EM SILÊNCIO" (o gateway nao fica publicando um
// estado que nao muda mais porque o sensor morreu).
void sensorPulsou();

// ---- utilitarios (ex.: para ler linhas JSON do Arduino) ----
String extrairTexto(const String& json, const String& chave);
int extrairInteiro(const String& json, const String& chave);
