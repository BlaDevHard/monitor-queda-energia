#pragma once
#include <Arduino.h>

/*
 * FONTE DE TEMPO — RTC interno do ESP32
 * =====================================
 * O projeto NAO usa modulo RTC externo. O tempo vem de tres
 * camadas, na ordem em que estao disponiveis:
 *
 *   1. NTP            — autoritativo; sincroniza no boot e quando
 *                       a rede volta (o roteador cai junto com a
 *                       rua, entao durante a queda nao ha NTP).
 *   2. RTC interno    — o system time do ESP32 continua correndo
 *                       enquanto a placa estiver energizada (pelo
 *                       power bank). E calibrado contra o cristal
 *                       de 40 MHz, entao nao deriva de forma
 *                       relevante durante uma queda.
 *   3. Checkpoint     — epoch salvo na flash por monitor_core.
 *                       Vale quando a placa perdeu a energia
 *                       (power bank esgotou) e volta sem NTP.
 *
 * A regra de ouro: `relogioAgora()` so vale como hora CONFIAVEL
 * depois do primeiro NTP neste ciclo de energia. Antes disso o
 * valor e uma estimativa e as mensagens sao marcadas como
 * "hora aproximada" — melhor marcar como aproximado do que
 * publicar uma hora inventada.
 */

// Inicia o NTP (nao bloqueia mais que ~3 s) e marca se ja temos
// hora confiavel. Chamar UMA vez no boot, antes de publicar.
void relogioIniciar();

// Chamar no loop: detecta o momento em que o NTP sincroniza.
void relogioLoop();

// true = o NTP ja sincronizou neste ciclo de energia.
bool relogioConfia();

// Epoch atual (UTC). Retorna 0 se nunca soube hora nenhuma.
// Se nao tem NTP mas ha um checkpoint, continua a partir dele
// (estimativa — confira relogioConfia() antes de publicar).
time_t relogioAgora();

// Continua a contagem a partir de um epoch salvo na flash.
// Chamar no boot, ANTES de relogioIniciar(), com o checkpoint.
void relogioDefinirBase(time_t epochSalvo);

// "dd/mm/aaaa hh:mm:ss" em GMT-3, ou "?" se nao ha hora.
String relogioFormatar(time_t t);

// "1h 2m 3s" / "2m 5s" / "45s"
String relogioFormatarDuracao(unsigned long segundos);

// true se o epoch veio do NTP; false se e estimativa/chute.
bool relogioValido(time_t t);
