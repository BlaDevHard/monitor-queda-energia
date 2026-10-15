
// ========================================
// SENSOR DE QUEDAS — Monitor de Energia
// Arduino Uno (ATmega328P)
//
// Hardware: ZMPT101B → pino A0
// Saída: UART 9600 → ESP-01
//
// NÃO calcula tensão. Apenas detecta se o
// ZMPT recebe energia da rua ou não:
//   sim -> "presente" (SERVICO ATIVO)
//   nao -> "ausente"  (QUEDA)
// ========================================

#include <Arduino.h>

// Amplitude minima (contagens pico-a-pico do ADC) para
// considerar que existe energia na rua.
// Calibrado em campo: com energia ~94, sem energia ~40
#define LIMITE_AMPLITUDE 65

// Leituras identicas seguidas para confirmar mudanca
// (~665ms cada -> ~2s de confirmacao, evita flicker)
#define CONFIRMACOES 3

// Janela de amostragem: 1500 leituras ~165ms (~10 ciclos de 60Hz)
#define AMOSTRAS 1500

bool bruta = false;           // ultima leitura bruta
int contador = 0;             // leituras identicas seguidas
bool confirmado = false;      // estado confirmado (enviado)
bool confirmadoValido = false;// ja houve 1a confirmacao
int amplitude = 0;            // p-p da ultima janela (diagnostico)

void setup() {
    Serial.begin(9600);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(A0, INPUT);
    delay(100);
    Serial.println("{\"boot\":\"Sensor OK\",\"versao\":\"2.1\"}");
}

// Detecta presenca de tensao alternada (sem calcular tensao)
bool detectarEnergia() {
    int minimo = 1023;
    int maximo = 0;
    for (int i = 0; i < AMOSTRAS; i++) {
        int leitura = analogRead(A0);
        if (leitura < minimo) minimo = leitura;
        if (leitura > maximo) maximo = leitura;
    }
    amplitude = maximo - minimo;
    return amplitude >= LIMITE_AMPLITUDE;
}

void loop() {
    bool leitura = detectarEnergia();

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

    // So envia a partir da 1a confirmacao (evita falso
    // estado logo apos o boot)
    if (confirmadoValido) {
        Serial.print("{\"energia\":\"");
        Serial.print(confirmado ? "presente" : "ausente");
        Serial.print("\",\"amp\":");
        Serial.print(amplitude);
        Serial.println("}");
    }

    // LED aceso = QUEDA (sem servico)
    digitalWrite(LED_BUILTIN, confirmado ? LOW : HIGH);

    delay(500);
}
