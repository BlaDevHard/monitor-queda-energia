
// ========================================
// MONITOR ENERGIA — SENSOR DE QUEDAS
// Arduino Uno (ATmega328P)
//
// Hardware: ZMPT101B → pino A0
// Saída: UART 9600 → ESP-01
//
// Detecta quedas < 110V e envia timestamp
// ========================================

#include <Arduino.h>

// ==== Configurações ====
#define LIMITE_QUEDA 110.0   // PRODIST para 127V
#define BUFFERSIZE 3000

float fator_calibracao = 0.85;
int estado_queda = 0;

void setup() {
    Serial.begin(9600);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(A0, INPUT);
    delay(100);
    Serial.println("{\"boot\":\"Monitor de Energia Sensor OK\",\"versao\":\"1.0\"}");
}

// Cálculo RMS (simplificado)
float lerRMS() {
    long soma = 0;
    for (int i = 0; i < 1500; i++) {
        int leitura = analogRead(A0);
        soma += (leitura - 512) * (leitura - 512);
        delayMicroseconds(5);
    }
    return sqrt(soma / 1500.0) * fator_calibracao;
}

void loop() {
    float vrms = lerRMS();
    
    Serial.print("{\"vrms\":");
    Serial.print(vrms);
    Serial.print(",\"status\":\"");
    Serial.print(vrms < LIMITE_QUEDA ? "QUEDA" : "OK");
    Serial.println("\"}");
    
    // Pisca LED se queda
    digitalWrite(LED_BUILTIN, (vrms < LIMITE_QUEDA) ? HIGH : LOW);
    
    delay(500);
}
