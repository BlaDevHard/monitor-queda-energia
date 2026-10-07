#include <Arduino.h>

#include <math.h>

#define NUM_AMOSTRAS 1500
#define LIMIAR_PRESENCA_ADC 20.0
#define LIMIAR_AUSENCIA_ADC 12.0
#define AMOSTRAS_PARA_CONFIRMAR 3

int8_t estadoEnergia = -1;
int8_t estadoCandidato = -1;
uint8_t leiturasCandidato = 0;

float lerRmsAc() {
    uint32_t soma = 0;
    uint32_t somaQuadrados = 0;

    for (uint16_t i = 0; i < NUM_AMOSTRAS; i++) {
        const uint16_t leitura = analogRead(A0);
        soma += leitura;
        somaQuadrados += (uint32_t)leitura * leitura;
        delayMicroseconds(5);
    }

    const float media = (float)soma / NUM_AMOSTRAS;
    const float mediaQuadrados = (float)somaQuadrados / NUM_AMOSTRAS;
    const float variancia = mediaQuadrados - media * media;
    return sqrtf(variancia > 0.0 ? variancia : 0.0);
}

void atualizaEstado(float rms) {
    int8_t novoCandidato = -1;

    if (rms >= LIMIAR_PRESENCA_ADC) {
        novoCandidato = 1;
    } else if (rms <= LIMIAR_AUSENCIA_ADC) {
        novoCandidato = 0;
    } else if (estadoEnergia != -1) {
        novoCandidato = estadoEnergia;
    }

    if (novoCandidato == -1) {
        leiturasCandidato = 0;
        estadoCandidato = -1;
        return;
    }

    if (novoCandidato != estadoCandidato) {
        estadoCandidato = novoCandidato;
        leiturasCandidato = 1;
    } else if (leiturasCandidato < AMOSTRAS_PARA_CONFIRMAR) {
        leiturasCandidato++;
    }

    if (leiturasCandidato >= AMOSTRAS_PARA_CONFIRMAR &&
        estadoEnergia != novoCandidato) {
        estadoEnergia = novoCandidato;
        digitalWrite(LED_BUILTIN, estadoEnergia ? HIGH : LOW);
        Serial.println(estadoEnergia ? "1" : "0");
    }
}

void setup() {
    Serial.begin(9600);
    pinMode(A0, INPUT);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
    atualizaEstado(lerRmsAc());
    delay(250);
}
