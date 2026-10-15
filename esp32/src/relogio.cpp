/*
 * FONTE DE TEMPO — RTC interno do ESP32 (sem modulo externo)
 * Ver relogio.h para a visao geral.
 */

#include "relogio.h"

#include <time.h>
#include <sys/time.h>

// Fuso do Brasil (GMT-3). O Brasil aboliu o horario de verao em
// 2019, entao nao ha deslocamento de verao a considerar.
static const long RELOGIO_GMT = -3L * 3600L;

// Epoch minimo aceito como "NTP ja respondeu": ~2023-11-14.
// Antes disso time(nullptr) devolve segundos desde o boot (ou 0),
// que e sempre um valor minusculo — o corte e seguro.
static const time_t EPOCH_MINIMO = 1700000000UL;

// ---- estado interno ----
static bool confia = false;          // NTP ja sincronizou neste ciclo
static time_t baseEstimada = 0;      // epoch de partida (checkpoint)
static unsigned long baseMillis = 0; // millis() na hora da partida

static bool ntpRespondeu() {
  return time(nullptr) > EPOCH_MINIMO;
}

void relogioDefinirBase(time_t epochSalvo) {
  if (epochSalvo <= 0) return;
  baseEstimada = epochSalvo;
  baseMillis = millis();
}

void relogioIniciar() {
  // configTime define o fuso (TZ) mesmo sem rede — entao ja
  // podemos formatar a hora estimada do checkpoint.
  configTime(RELOGIO_GMT, 0, "pool.ntp.org", "time.google.com", "a.ntp.br");

  // Espera um pouco pelo 1o sync, mas NAO trava o boot: se o
  // roteador estiver morto (queda em andamento) seguimos com a
  // hora estimada e marcamos tudo como aproximado.
  for (int i = 0; i < 12 && !ntpRespondeu(); i++) delay(250);

  confia = ntpRespondeu();
  if (confia) {
    baseEstimada = 0;                 // NTP mandou: dispensa a estimativa
    Serial.println("Relogio: sincronizado com NTP");
  } else {
    Serial.println("Relogio: SEM NTP — usando hora estimada (aproximada)");
  }
}

void relogioLoop() {
  if (confia) return;
  if (!ntpRespondeu()) return;

  confia = true;
  baseEstimada = 0;
  Serial.println("Relogio: NTP sincronizado apos boot (hora confiavel)");
}

bool relogioConfia() {
  return confia;
}

time_t relogioAgora() {
  if (ntpRespondeu()) return time(nullptr);
  if (baseEstimada > 0) return baseEstimada + (millis() - baseMillis) / 1000UL;
  return 0;
}

bool relogioValido(time_t t) {
  // Vale como hora exata apenas se o NTP ja sincronizou neste
  // ciclo de energia. Sem isso, o epoch e estimativa do checkpoint.
  return confia && t > 0;
}

String relogioFormatar(time_t t) {
  if (t <= 0) return String("?");
  struct tm* info = localtime(&t);
  if (info == nullptr) return String("?");
  char buf[40];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", info);
  return String(buf);
}

String relogioFormatarDuracao(unsigned long segundos) {
  unsigned long seg = segundos % 60;
  unsigned long min = (segundos / 60) % 60;
  unsigned long hora = segundos / 3600;

  if (hora > 0) return String(hora) + "h " + String(min) + "m " + String(seg) + "s";
  if (min > 0) return String(min) + "m " + String(seg) + "s";
  return String(seg) + "s";
}
