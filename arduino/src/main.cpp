// =============================================================
// SENSOR DE QUEDAS — Monitor de Energia   (v3.1)
// Arduino Uno (ATmega328P)
//
// COMO DETECTA — DIRETO NA RUA
// ----------------------------
// O ZMPT101B esta ligado na rede e sua saida cai no A0. Nao ha
// intermediario: o Arduino ve a alternada da rua, chega e vai.
//
//   rua ALIDADE      -> sinal alternada no A0, amplitude ~424
//   rua SEM energia  -> so ruido, amplitude ~7
//
// E o mesmo metodo da v1, calibrado em campo: uma janela de
// 1500 amostras (~165 ms, ~10 ciclos de 60 Hz) e a amplitude
// pico-a-pico decide.
//
// O QUE SAIU DAQUI (v3.0 -> v3.1)
// -------------------------------
// A leitura da Vcc pelo bandgap interno foi REMOVIDA. Ela
// inferia a queda pela troca tomada -> power bank na propria
// alimentacao do chip. Isso pedia calibracao de limiar, dependia
// do power bank assumir limpo (havia risco de pass-through), nao
// media a rua e ainda quebrava se o banco mudasse de tensao.
// Com o ZMPT medindo a rua, aquilo era redundante e so trazia
// ponto de falha. Foram tambem removidos os campos "fonte" e "vcc".
//
// Saida: UART 9600 -> ESP32
//   {"boot":"Sensor OK","versao":"3.1"}
//   {"energia":"presente|ausente","amp":N}
//
// Linhas de diagnostico comecam com "#" — o ESP32 ignora.
// LED aceso = QUEDA (sem servico), como na v1.
//
// AVISO: o lado de ENTRADA do ZMPT fica na rede (127/220 V).
// Nao toque nos terminais de entrada com a placa alimentada.
// =============================================================

#include <Arduino.h>

// ---------------- calibracao -----------------
// Amplitude minima (contagens pico-a-pico do ADC) para considerar
// que existe energia na rua.
//
// Medido nesta instalacao:
//   rua ligada    -> amp ~424
//   rua desligada -> amp ~7
//
// O limiar 65 fica com folga dos dois lados: 9x acima do ruido e
// 6.5x abaixo do sinal. E o mesmo limiar de campo da v1.
#define LIMITE_AMPLITUDE 65

// Leituras identicas seguidas para confirmar a mudanca.
// Com 500 ms por ciclo, 3 confirmacoes = ~1,5 s (mesmo debounce
// da v1, que aguentava campo sem flicker).
#define CONFIRMACOES 3

// Janela de amostragem: 1500 leituras ~165 ms (~10 ciclos de 60 Hz)
#define AMOSTRAS 1500

// Limiar de cruzamento para medir a FREQUENCIA (diagnostico).
// A saida do ZMPT fica centrada em ~Vcc/2, entao 511 e o meio.
#define LIMIAR_CRUZ 511

// Ritmo de leitura (ms). Mantido da v1.
#define CICLO_MS 500

// -------------------------------------------------------------
bool bruta = false;             // ultima leitura bruta
int contador = 0;               // leituras identicas seguidas
bool confirmado = false;        // estado confirmado
bool confirmadoValido = false;  // ja houve a 1a confirmacao
int amplitude = 0;              // p-p da ultima janela
int minimo = 0, maximo = 0;     // bordas da ultima janela
int saturadas = 0;              // amostras na borda (0 ou 1023)

// =============================================================
// JANELA DE AMOSTRAGEM
// =============================================================
// Uma passada so: min/max (amplitude) E subidas acima do limiar
// (frequencia). Medir os dois na mesma passada economiza ~165 ms
// por ciclo e garante que os dois numeros descrevem o MESMO
// trecho de sinal.
// =============================================================
static int medirJanela(unsigned long& usDecorrido) {
    minimo = 1023;
    maximo = 0;
    saturadas = 0;
    int anterior = analogRead(A0);
    int subidas = 0;

    unsigned long t0 = micros();
    for (int i = 0; i < AMOSTRAS; i++) {
        int leitura = analogRead(A0);
        if (leitura < minimo) minimo = leitura;
        if (leitura > maximo) maximo = leitura;
        if (leitura <= 0 || leitura >= 1023) saturadas++;
        if (anterior < LIMIAR_CRUZ && leitura >= LIMIAR_CRUZ) subidas++;
        anterior = leitura;
    }
    usDecorrido = micros() - t0;

    amplitude = maximo - minimo;
    return subidas;
}

void setup() {
    Serial.begin(9600);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(A0, INPUT);
    delay(100);

    Serial.println(F("{\"boot\":\"Sensor OK\",\"versao\":\"3.1\"}"));
    Serial.println(F("# v3.1: ZMPT no A0 — deteccao direta da rua"));
    Serial.println(F("# (metodo Vcc/jack/USB removido)"));
    Serial.print(F("# limiar amplitude="));
    Serial.println(LIMITE_AMPLITUDE);
}

void loop() {
    unsigned long janelaUs = 0;
    int subidas = medirJanela(janelaUs);

    // Frequentia so faz sentido com sinal de verdade. Sem rua, o
    // cruzamento e sorteio do ruido — por isso aparece so em cima.
    long freqCx = -1;              // frequencia x10 (601 = 60.1 Hz)
    if (amplitude >= LIMITE_AMPLITUDE && subidas > 0 && janelaUs > 0)
        freqCx = (long) ((subidas * 10000000UL) / janelaUs);

    bool bruto = amplitude >= LIMITE_AMPLITUDE;

    // Debounce: exige CONFIRMACOES leituras identicas seguidas
    if (bruto == bruta) {
        if (contador < CONFIRMACOES) contador++;
    } else {
        bruta = bruto;
        contador = 1;
    }
    if (contador >= CONFIRMACOES) {
        confirmado = bruta;
        confirmadoValido = true;
    }

    // Diagnostico (o ESP32 ignora linhas que nao comecam com "{")
    Serial.print(F("#ZMPT amp="));
    Serial.print(amplitude);
    Serial.print(F(" [min="));
    Serial.print(minimo);
    Serial.print(F(" max="));
    Serial.print(maximo);
    Serial.print(F("]"));
    if (freqCx >= 0) {
        Serial.print(F(" f="));
        Serial.print(freqCx / 10);
        Serial.print('.');
        Serial.print(freqCx % 10);
        Serial.print(F("Hz"));
    }
    Serial.print(F("  bruto="));
    Serial.print(bruto ? F("ENERGIA") : F("SEM-ENERGIA"));
    Serial.print(F("  conf="));
    Serial.print(confirmadoValido ? contador : 0);
    if (saturadas > AMOSTRAS / 10) {
        Serial.print(F("  !! SATURADO (sinal tocando 0/1023)"));
    }
    Serial.println();

    // Estado no contrato do gateway. So a partir da 1a confirmacao,
    // igual a v1: evita publicar um estado errado logo no boot.
    if (confirmadoValido) {
        Serial.print(F("{\"energia\":\""));
        Serial.print(confirmado ? F("presente") : F("ausente"));
        Serial.print(F("\",\"amp\":"));
        Serial.print(amplitude);
        Serial.println(F("}"));
    }

    // LED aceso = QUEDA (sem servico)
    digitalWrite(LED_BUILTIN, confirmado ? HIGH : LOW);

    delay(CICLO_MS);
}
