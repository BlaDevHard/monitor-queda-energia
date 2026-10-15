/*
 * OPCAO 2 — PLACA UNICA (sensor + gateway no ESP32)
 * =================================================
 * Le o ZMPT101B direto no ADC e faz todo o resto:
 * Discord + Pushbullet + NTP + MQTT + servidor HTTP.
 * Nao ha Arduino nem UART: um chip so.
 *
 * CONEXOES (VER esp32/README.md ANTES DE LIGAR):
 *   ZMPT101B OUT -> divisor 10k/10k -> GPIO34 (ADC1)
 *   ZMPT101B VCC -> 3.3V*  (*confirmar pico do OUT com multimetro
 *                           antes da instalacao permanente)
 *   ZMPT101B GND -> GND
 *
 *   O sinal do ZMPT e AC centrado em 0V e o ESP32 NAO aceita 5V:
 *   o divisor e OBRIGATORIO. O firmware vigia o ADC e alerta
 *   "ADC SATURADO" se o sinal tocar as bordas (0/4095).
 *
 * DETECCAO POR PRESENCA (sem tensao, sem calibracao de voltagem):
 *   - janela de ~166 ms (~10 ciclos de 60 Hz) -> amplitude pico-a-pico
 *   - amp >= LIMITE_AMPLITUDE  -> "presente"
 *   - debounce: 3 leituras identicas seguidas (~2 s), igual ao Arduino
 *
 * O limiar LIMITE_AMPLITUDE esta em contagens do ADC do ESP32
 * (12 bits, 3.3 V). O padrao 400 ~= 65 do Arduino (10 bits, 5 V).
 * Ajuste observando o "AMP=" no monitor serial — nao precisa de
 * multimedro (ver esp32/README.md).
 *
 * AVISO: errar o limiar nao danifica hardware (e so software).
 * O risco eletrico e o sinal exceder 0..3.3V no pino — coberto
 * pelo divisor + pela vigilancia de saturacao abaixo.
 */

#include "monitor_core.h"
#include "config.h"

static bool bruta = false;            // ultima leitura bruta
static int contador = 0;              // leituras identicas seguidas
static bool confirmado = false;       // estado confirmado
static bool confirmadoValido = false; // ja houve a 1a confirmacao
static int amplitude = 0;             // p-p da ultima janela
static int minJanela = 4095;          // minimo da ultima janela (saturacao)
static int maxJanela = 0;             // maximo da ultima janela (saturacao)
static unsigned long ultimoCiclo = 0;

// Saturacao = sinal tocando as bordas do ADC (0 ou 4095): o sinal
// do ZMPT esta fora de 0..3.3V. Exige JANELAS_ALERTA janelas
// seguidas para não alarmar por um pico isolado.
static int janelasSaturadas = 0;
static bool alertaSaturacaoEnviado = false;   // 1 alerta por episodio

void setup() {
  coreIniciar("unificado_adc");

  pinMode(PINO_ZMPT, INPUT);
  analogSetAttenuation(ADC_11db);   // escala cheia ~3.3 V (padrao)

  Serial.println("ADC: ZMPT no GPIO" + String(PINO_ZMPT) +
                 " | limiar=" + String(LIMITE_AMPLITUDE) +
                 " | debounce=" + String(CONFIRMACOES));
}

// Detecta presenca de tensao alternada (sem calcular tensao):
// amostra uma janela de JANELA_MS e mede amplitude pico-a-pico.
static bool detectarEnergia() {
  unsigned long inicio = millis();
  int minimo = 4095;
  int maximo = 0;

  while (millis() - inicio < JANELA_MS) {
    int leitura = analogRead(PINO_ZMPT);
    if (leitura < minimo) minimo = leitura;
    if (leitura > maximo) maximo = leitura;
  }

  minJanela = minimo;
  maxJanela = maximo;
  amplitude = maximo - minimo;
  return amplitude >= LIMITE_AMPLITUDE;
}

// Sinal nas bordas do ADC = fora da faixa 0..3.3V no pino.
// Envia UM alerta por episodio (laranja, no Discord/Pushbullet).
static void vigiarSaturacao() {
  bool nasBordas = (minJanela <= 1) || (maxJanela >= 4094);

  if (nasBordas) {
    if (janelasSaturadas < JANELAS_ALERTA) janelasSaturadas++;
    if (janelasSaturadas >= JANELAS_ALERTA) {
      sinalSaturado = true;
      if (!alertaSaturacaoEnviado) {
        alertaSaturacaoEnviado = true;
        String msg = "O sinal do ZMPT esta tocando as bordas do ADC";
        msg += " (min=" + String(minJanela) + " max=" + String(maxJanela) + ").\n";
        msg += "O sinal esta fora de 0..3.3V no GPIO" + String(PINO_ZMPT) + ".\n";
        msg += "Confira o divisor/alimentacao do ZMPT antes de continuar.";
        Serial.println("ALERTA: ADC saturado (min=" + String(minJanela) +
                       " max=" + String(maxJanela) + ")");
        alertaHardware("⚠️ ADC SATURADO", msg);
      }
    }
  } else {
    janelasSaturadas = 0;
    if (sinalSaturado) {
      sinalSaturado = false;
      alertaSaturacaoEnviado = false;   // proximo episodio alerta de novo
      Serial.println("ADC voltou a faixa normal");
    }
  }
}

void loop() {
  coreLoop();

  // Ritmo do Arduino: ~665 ms por ciclo (janela + espera).
  // A espera e fora do delay: coreLoop() continua respondendo
  // HTTP/MQTT entre um ciclo e outro.
  if (millis() - ultimoCiclo < CICLO_MS) return;
  ultimoCiclo = millis();

  bool leitura = detectarEnergia();
  vigiarSaturacao();

  // Debounce: exige CONFIRMACOES leituras identicas seguidas
  if (leitura == bruta) {
    if (contador < CONFIRMACOES) contador++;
  } else {
    bruta = leitura;
    contador = 1;
  }
  if (contador >= CONFIRMACOES) {
    confirmado = bruta;
    confirmadoValido = true;
  }

  // So publica a partir da 1a confirmacao (evita falso estado
  // logo apos o boot) — igual ao sensor Arduino.
  if (confirmadoValido) {
    processarEstado(confirmado ? "presente" : "ausente", amplitude);
  } else {
    Serial.println("AMP=" + String(amplitude) + " (aguardando confirmacao " +
                   String(contador) + "/" + String(CONFIRMACOES) + ")");
  }

  // Diagnostico de faixa (sempre visivel no serial):
  // min/max nas bordas (0/4095) = sinal fora de 0..3.3V
  Serial.println("  janela: min=" + String(minJanela) +
                 " max=" + String(maxJanela) +
                 (sinalSaturado ? "  <-- SATURADO" : ""));
}
