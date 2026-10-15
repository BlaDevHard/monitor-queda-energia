/*
 * SONDA ZMPT — a rua chega pelo ZMPT101B?
 * =========================================
 * O RTC saiu do sistema. Agora a pergunta e: o ZMPT esta vendo
 * energia da rua? Esta sonda le o A0 (saida do ZMPT) com a MESMA
 * tecnica que foi calibrada em campo na v2.1:
 *
 *   janela de 1500 amostras (~165 ms ~ 10 ciclos de 60 Hz)
 *   amplitude = max - min          (contagens 0..1023)
 *
 *   campo: com energia ~94, sem energia ~40  ->  limiar 65
 *
 * Alem disso conta os cruzamentos da linha central numa 2a janela
 * para estimar a FREQUENCIA. Se bater ~60 Hz, e a rua mesmo —
 * ruido nao tem frequencia definida. E o que separa "sem rua" de
 * "sinal fraco".
 *
 * Saida (9600):
 *   #ZMPT ...  diagnostico — o gateway ignora (nao comeca com "{")
 *   {"energia":"presente|ausente","amp":N}   contrato v2, o gateway reage
 *
 * LED aceso = QUEDA (sem servico), como na v2.1.
 *
 * AVISO DE SEGURANCA: a ENTRADA do ZMPT fica na rede (127/220 V).
 * Nao toque nos terminais de entrada com a placa alimentada.
 *
 * Gravar: pio run -e zmpt_teste -t upload
 */

#include <Arduino.h>

#define AMOSTRAS         1500   // ~165 ms  (~10 ciclos de 60 Hz)
#define LIMITE_AMPLITUDE 65     // calibrado em campo (v2.1)
#define CONFIRMACOES     3      // debounce ~1 s
#define CICLO_MS         150    // respiro entre ciclos

// ---------------- Vcc pelo bandgap interno -----------------------
// Mesma tecnica da v3. Serve pra saber de onde a placa esta
// alimentada enquanto olha o ZMPT (duas pistas juntas).
#define BANDGAP_AMOSTRAS 8
#define BANDGAP_MV 1100UL

static unsigned int lerVcc_mV() {
    ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
    delay(2);
    (void) ADCW;
    unsigned long soma = 0;
    for (uint8_t i = 0; i < BANDGAP_AMOSTRAS; i++) {
        ADCSRA |= _BV(ADSC);
        while (bit_is_set(ADCSRA, ADSC)) { }
        soma += ADCW;
    }
    unsigned int media = (unsigned int) (soma / BANDGAP_AMOSTRAS);
    if (media == 0) media = 1;
    // volta o conversor pro canal 0 (= A0; A0 e o pino 14, e o
    // canal ADC e 14-14=0). O analogRead ja reescreve o ADMUX a
    // cada chamada, entao e so por clareza.
    ADMUX = _BV(REFS0);   // REFS0 = AVcc como referencia, canal 0 = A0
    return (unsigned int) ((BANDGAP_MV * 1024UL) / media);
}

// ---------------- medicao do ZMPT --------------------------------
static int minimo = 0, maximo = 0, amplitude = 0;
static unsigned long janelaUs = 0;

// Janela 1: min/max/amplitude.
static void medirJanela() {
    minimo = 1023;
    maximo = 0;
    unsigned long t0 = micros();
    for (int i = 0; i < AMOSTRAS; i++) {
        int leitura = analogRead(A0);
        if (leitura < minimo) minimo = leitura;
        if (leitura > maximo) maximo = leitura;
    }
    janelaUs = micros() - t0;
    amplitude = maximo - minimo;
}

// Janela 2: subidas acima da linha central -> frequencia.
// Roda SEMPRE: e o que separa "sinal fraco" (existe 60 Hz, so que
// abaixo do limiar -> potenciometro no minimo) de "sinal ausente"
// (so tremulacao, frequencia sem sentido).
static int contarCruzamentos() {
    int meio = (minimo + maximo) / 2;
    int anterior = analogRead(A0);
    int n = 0;
    unsigned long t0 = micros();
    for (int i = 0; i < AMOSTRAS; i++) {
        int leitura = analogRead(A0);
        if (anterior < meio && leitura >= meio) n++;
        anterior = leitura;
    }
    janelaUs = micros() - t0;
    return n;
}

// ---------------- o A0 esta mesmo ligado ao ZMPT? ----------------
// Liga o pull-up interno do A0 (~35k p/ Vcc) e ve se o nivel sobe.
//
//   nivel MUDA pouco  -> o modulo SEGURA o pino (baixa impedancia):
//                        o fio esta ligado e o modulo trabalha.
//   nivel SOBE muito  -> pino flutuando: o fio NAO chega ao A0.
//
// O pull-up limita a corrente em ~0,1 mA — inofensivo para a saida
// do modulo (burden tipico de dezenas/centenas de ohms).
static void medirNivelA0(int& semPull, int& comPull) {
    pinMode(A0, INPUT);
    delay(5);
    medirJanela();
    semPull = (minimo + maximo) / 2;

    pinMode(A0, INPUT_PULLUP);
    delay(5);
    medirJanela();
    comPull = (minimo + maximo) / 2;

    pinMode(A0, INPUT);   // volta ao normal antes das leituras normais
    delay(5);
}

// ---------------- maquina de estados (v2.1) ----------------------
static bool bruta = false;             // ultima leitura crua
static int  contador = 0;              // leituras identicas seguidas
static bool confirmado = false;        // estado confirmado (enviado)
static bool confirmadoValido = false;  // ja houve a 1a confirmacao

static void imprimirAviso() {
    if (amplitude == 0) {
        Serial.print(F(" !! A0 PARADO (ZMPT sem 5 V ou saida desconectada)"));
    } else if (maximo >= 1022) {
        Serial.print(F(" !! SATURADO (baixar o potenciometro do modulo)"));
    } else if (!bruta && amplitude >= 30 && amplitude < LIMITE_AMPLITUDE) {
        Serial.print(F(" (fraco: rua sem energia, ou pot. baixo demais)"));
    }
}

void setup() {
    Serial.begin(9600);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(A0, INPUT);
    delay(100);

    Serial.println(F("========================================="));
    Serial.println(F(" SONDA ZMPT — teste de chegada da rua"));
    Serial.println(F(" A0 = saida do ZMPT101B"));
    Serial.println(F(" janela 1500 amostras, limiar amplitude 65"));
    Serial.println(F(" campo: com rua ~94 / sem rua ~40"));
    Serial.println(F(" LINHAS COM # SAO DIAGNOSTICO"));
    Serial.println(F("========================================="));

    // estado do modulo logo de cara (nao espera o debounce)
    medirJanela();
    Serial.print(F("#ZMPT inicial amp="));
    Serial.print(amplitude);
    Serial.print(F(" min="));
    Serial.print(minimo);
    Serial.print(F(" max="));
    Serial.println(maximo);
    imprimirAviso();
    Serial.println();

    // o A0 chega ao modulo? (pull-up de teste)
    int semPull = 0, comPull = 0;
    medirNivelA0(semPull, comPull);
    Serial.print(F("#A0 ligado? nivel sem pull-up="));
    Serial.print(semPull);
    Serial.print(F(" com pull-up="));
    Serial.print(comPull);
    Serial.print(F(" -> "));
    if (abs(comPull - semPull) < 60) {
        Serial.println(F("LIGADO (modulo segura o pino)"));
    } else if (comPull > 900) {
        Serial.println(F("FLUTUANDO (fio nao chega ao A0)"));
    } else {
        Serial.println(F("INCONCLUSIVO"));
    }
    Serial.println(F("#(freq ~60Hz com amp<65 = potenciometro no minimo)"));
    Serial.println();
}

void loop() {
    // ---- janela 1: amplitude ----
    medirJanela();
    bool bruto = amplitude >= LIMITE_AMPLITUDE;

    // ---- janela 2: frequencia (SEMPRE — ve a nota no cabecalho) ----
    int cruzamentos = contarCruzamentos();
    long freqCx = -1;                   // frequencia x10 (ex.: 601 = 60.1 Hz)
    if (cruzamentos > 0 && janelaUs > 0)
        freqCx = (long) ((cruzamentos * 10000000UL) / janelaUs);
    bool freqConf = (bruto && freqCx >= 500 && freqCx <= 700);  // 50..70 Hz

    // ---- debounce (igual a v2.1) ----
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

    unsigned int vcc = lerVcc_mV();

    // ---- diagnostico (linha com "#" -> gateway ignora) ----
    Serial.print(F("#ZMPT amp="));
    Serial.print(amplitude);
    Serial.print(F(" [min="));
    Serial.print(minimo);
    Serial.print(F(" max="));
    Serial.print(maximo);
    Serial.print(F("]"));
    if (cruzamentos >= 0) {
        Serial.print(F(" cruz="));
        Serial.print(cruzamentos);
        Serial.print(F(" f="));
        Serial.print(freqCx / 10);
        Serial.print('.');
        Serial.print(freqCx % 10);
        Serial.print(F("Hz"));
        if (freqConf) Serial.print(F(" CONF"));
        else if (!bruto && freqCx >= 500 && freqCx <= 700)
            Serial.print(F(" FRACO (60Hz abaixo do limiar)"));
    }
    Serial.print(F(" Vcc="));
    Serial.print(vcc);
    Serial.print(F("mV  bruto="));
    Serial.print(bruto ? F("ENERGIA") : F("SEM-ENERGIA"));
    Serial.print(F("  conf="));
    if (confirmadoValido) {
        Serial.print(confirmado ? F("ENERGIA") : F("SEM-ENERGIA"));
    } else {
        Serial.print(F("aguardando"));
    }
    imprimirAviso();
    Serial.println();

    // ---- estado no contrato v2 (o gateway reage a isto) ----
    // So a partir da 1a confirmacao, como na v2.1: evita mandar
    // um estado logo no boot.
    if (confirmadoValido) {
        Serial.print(F("{\"energia\":\""));
        Serial.print(confirmado ? F("presente") : F("ausente"));
        Serial.print(F("\",\"amp\":"));
        Serial.print(amplitude);
        Serial.println(F("}"));
    }

    // LED aceso = QUEDA
    digitalWrite(LED_BUILTIN, (confirmadoValido && confirmado) ? LOW : HIGH);

    delay(CICLO_MS);
}
