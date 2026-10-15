// =============================================================
// SONDA DE CICLO — teste QUEDA -> SERVICO ATIVO
// Arduino Uno (ATmega328P)   |   NAO e firmware de producao
//
// POR QUE EXISTE
// --------------
// A v3.1 mudou o detector (ZMPT direto no A0) e o protocolo
// (saiu "fonte"/"vcc"). Tudo foi verificado no hardware, MENOS o
// caminho completo: presente -> ausente -> presente. Esse caminho
// so roda quando a rua cai de verdade — e cair a rua de proposito
// nao e opcao (o lado de entrada do ZMPT esta em 127/220 V).
//
// COMO FUNCIONA
// -------------
// Nao le o ZMPT. Simula a saida do main.cpp real, com o MESMO
// contrato e o MESMO debounce (CONFIRMACOES=3 -> ~1,5 s), trocando
// o limiar de amplitude por um cronograma fixo:
//
//   presente 15 s            -> ESP32 estabiliza / ESTADO INICIAL
//   3x [ ausente 20 s | presente 20 s ]  -> QUEDA e SERVICO ATIVO
//   presente permanente      -> encerra
//
// amplitudes copiadas da medicao real: 421 com rua, 7 sem rua.
//
// A saida e identica a do firmware real, entao o ESP32 nao precisa
// mudar nada:
//   {"boot":"Sensor OK","versao":"3.1-ciclo"}
//   {"energia":"presente|ausente","amp":N}
//   #... diagnostico (ignorado pelo ESP32)
//
// LED aceso = QUEDA, igual ao firmware real.
//
// USO:  pio run -e ciclo_teste -t upload
// Depois REGRAVAR o firmware de verdade:  pio run -e uno -t upload
//
// AVISO: enquanto esta gravado, o Arduino NAO esta medindo a rua.
// =============================================================

#include <Arduino.h>

// ---------------- calibracao (igual a v3.1) -----------------
// Debounce: 3 leituras identicas seguidas = ~1,5 s a 500 ms/ciclo.
#define CONFIRMACOES 3

// Ritmo de leitura (ms), identico ao main.cpp.
#define CICLO_MS 500

// ---------------- cronograma da simulacao -------------------
// Amplitude reportada em cada estado. Copiada da instalacao real.
#define AMP_COM_RUA 421
#define AMP_SEM_RUA 7

#define MS_PRESENTE_INICIAL 15000UL   // tempo para o ESP32 subir
#define MS_AUSENTE 20000UL            // duracao da "queda"
#define MS_PRESENTE 20000UL           // duracao do "servico ativo"
#define CICLOS 3                      // quantas quedas simular

// -------------------------------------------------------------
bool bruta = false;             // leitura bruta do cronograma
int contador = 0;               // leituras identicas seguidas
bool confirmado = false;        // estado confirmado
bool confirmadoValido = false;  // ja houve a 1a confirmacao

// Cronograma
unsigned long tInicio = 0;      // ms desde o boot
bool faseAusente = false;       // fase atual do cronograma
bool testeEncerrado = false;    // terminou CICLOS vezes

// Devolve o estado BRUTO que o cronograma manda nesta fase.
// Atualiza "faseAusente" junto, para o diagnostico bater.
bool estadoDaFase() {
    if (testeEncerrado) {
        faseAusente = false;                    // preso em presente
        return true;
    }

    unsigned long t = millis() - tInicio;
    if (t < MS_PRESENTE_INICIAL) return true;     // margem de boot

    // Dentro de um ciclo: [ausente][presente]
    unsigned long desde = t - MS_PRESENTE_INICIAL;
    unsigned long cicloLen = MS_AUSENTE + MS_PRESENTE;
    unsigned long dentro = desde % cicloLen;
    int ciclo = (int) (desde / cicloLen) + 1;

    if (ciclo > CICLOS) {
        testeEncerrado = true;
        return true;
    }
    faseAusente = (dentro < MS_AUSENTE);
    return !faseAusente;
}

void setup() {
    Serial.begin(9600);
    pinMode(LED_BUILTIN, OUTPUT);
    delay(100);

    Serial.println(F("{\"boot\":\"Sensor OK\",\"versao\":\"3.1-ciclo\"}"));
    Serial.println(F("# SONDA DE CICLO — simulando a saida do ZMPT"));
    Serial.println(F("# NAO esta medindo a rua; regrave com 'pio run -e uno'"));
    Serial.print(F("# "));
    Serial.print(CICLOS);
    Serial.println(F(" quedas de "));
    Serial.print(MS_AUSENTE / 1000);
    Serial.println(F(" s cada"));

    tInicio = millis();
}

void loop() {
    bool bruto = estadoDaFase();

    // Debounce identico ao main.cpp real: exige CONFIRMACOES
    // leituras identicas seguidas antes de mudar o estado.
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

    int amplitude = bruto ? AMP_COM_RUA : AMP_SEM_RUA;

    // Diagnostico (o ESP32 ignora linhas que nao comecam com "{")
    Serial.print(F("#SIM t="));
    Serial.print((millis() - tInicio) / 1000);
    Serial.print(F("s fase="));
    Serial.print(faseAusente && !testeEncerrado ? F("AUSENTE") : F("PRESENTE"));
    Serial.print(F(" amp="));
    Serial.print(amplitude);
    Serial.print(F("  bruto="));
    Serial.print(bruto ? F("ENERGIA") : F("SEM-ENERGIA"));
    Serial.print(F("  conf="));
    Serial.print(confirmadoValido ? contador : 0);
    if (testeEncerrado) Serial.print(F("  fim=1"));
    Serial.println();

    // Estado no contrato do gateway — igual ao firmware real.
    if (confirmadoValido) {
        Serial.print(F("{\"energia\":\""));
        Serial.print(confirmado ? F("presente") : F("ausente"));
        Serial.print(F("\",\"amp\":"));
        Serial.print(amplitude);
        Serial.println(F("}"));
    }

    // LED aceso = QUEDA (sem servico), como no firmware real
    digitalWrite(LED_BUILTIN, confirmado ? HIGH : LOW);

    delay(CICLO_MS);
}
