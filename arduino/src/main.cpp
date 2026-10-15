// =============================================================
// SENSOR DE QUEDAS — Monitor de Energia   (v3.2)
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
// 500 amostras (~55 ms, ~3,3 ciclos de 60 Hz) e a amplitude
// pico-a-pico decide.
//
// O QUE MUDOU NA v3.2 — QUEDA CURTA
// ---------------------------------
// A v3.1 so via queda longa. A janela era de 165 ms e o Arduino
// exigia 3 leituras identicas antes de falar: um pulso de 300 ms
// morria antes de ser reportado, e o ESP32 ficava sem saber que
// ele existiu.
//
// A v3.2 enxerga o pulso inteiro:
//   - janela caiu de 165 ms para 55 ms (pulso de 100 ms ocupa ~2)
//   - o Arduino parou de filtrar (CONFIRMACOES 1) e passou a
//     reportar CADA pulso com a duracao medida
//   - o debounce que move o Discord/Pushbullet foi para o ESP32,
//     que tem relogio, memoria e contexto para juntar varios
//     pulsos num relatorio so
//
// Contrato da UART (9600 -> ESP32):
//   {"boot":"Sensor OK","versao":"3.2"}
//   {"energia":"presente|ausente","amp":N}
//   {"pulso":1,"dur_ms":N,"amp_min":N}          <- pulso terminou
//   {"pulso":1,"abrindo":1,"amp":N}             <- pulso em curso
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

// Debounce do Arduino. ANTES era 3 — e isso engolia queda curta: um
// pulso de 300 ms nunca sobrevivia a 3 leituras identicas de 500 ms,
// entao o ESP32 nunca via o pulso existir.
//
// Agora e 1: o Arduino reporta TUDO que observa, sem filtrar. Quem
// decide o que vira alerta e o ESP32, que tem relogio, memoria e
// contexto (ver "relatorio de pulso" em esp32/src/monitor_core.cpp).
//
// O debounce NAO sumiu — mudou de lugar. O estado
// "presente|ausente" que move Discord/Pushbullet continua exigindo
// 3 confirmacoes, la no ESP32.
#define CONFIRMACOES 1

// Janela de amostragem: 500 leituras ~55 ms (~3,3 ciclos de 60 Hz)
//
// ANTES era 1500 (~165 ms). Mudou para capturar QUEDA CURTA: com
// 165 ms de janela, um pulso de 100 ms cabia inteiro dentro de UMA
// amostragem e a amplitude media continuava alta — o pulso sumia
// antes de sair do Arduino. Com 55 ms um pulso de 100 ms ocupa ~2
// janelas e aparece.
//
// CUSTO: 55 ms cobre so 3,3 periodos de 60 Hz, entao a FREQUENCIA
// medida numa unica janela oscila (53..71 Hz em campo). Para
// recuperar o numero confiavel, medimos f como media de 20 janelas
// (~1 s) — ver medirFrequencia(). Isso nao afeta a deteccao de
// pulso, que olha so a amplitude.
#define AMOSTRAS 500

// Limiar de cruzamento para medir a FREQUENCIA (diagnostico).
// A saida do ZMPT fica centrada em ~Vcc/2, entao 511 e o meio.
#define LIMIAR_CRUZ 511

// Ritmo de leitura (ms).
//
// ATENCAO — isto NAO e um delay() fixo. e o ALVO de um ciclo que se
// auto-ajusta. Antes o loop fazia delay(500) e o ciclo real levava
// ~665 ms (165 de janela + 500 de delay). Um pulso de 300 ms nascia
// e morria DENTRO do delay, e o Arduino nunca chegava a ve-lo. E
// justamente o caso que a v3.2 existe para pegar.
//
// O ciclo mede quando comecou e so espera o que falta. A janela de
// 55 ms e o que consome; nao ha folga programada em cima.
#define CICLO_MS 60

// -----------------------------------------------------------------
// IMPRESSAO COMPARTILHADA
// -----------------------------------------------------------------
// O gargalo deste firmware NAO e o ADC — e a serial. 9600 baud da
// 960 bytes/s, e o Arduino nao tem FIFO de transmissao: enquanto o
// byte nao sai, ele fica travado em espera, cego para a rede.
//
// Uma linha de diagnostico + uma de JSON sao ~101 ms. Se isso
// rodasse a cada ciclo, o firmware passaria ~58% do tempo BLOCKADO
// esperando serial — e um pulso de 200 ms passaria embaixo do radar
// justamente por causa da propria telemetria.
//
// Por isso: o diagnostico sai so em DIAG_MS, e o JSON do estado so
// quando o estado MUDA (ou no heartbeat). O tempo entre eles fica
// livre para o Arduino olhar a rede.
#define DIAG_MS 2000          // linhas # de diagnostico
#define HEARTBEAT_MS 5000     // reenvio do estado mesmo sem mudanca
unsigned long ultimaDiag = 0;
unsigned long ultimoEstado = 0;

// -------------------------------------------------------------
bool bruta = false;             // ultima leitura bruta
int contador = 0;               // leituras identicas seguidas
bool confirmado = false;        // estado confirmado
bool confirmadoValido = false;  // ja houve a 1a confirmacao
int amplitude = 0;              // p-p da ultima janela
int minimo = 0, maximo = 0;     // bordas da ultima janela
bool estadoEnviado = false;     // ja mandamos o estado no UART
bool ultimoEstadoEnviado = false;  // ultimo estado que saiu
#define PULSO_ABRINDO_MS 200    // "pulso em curso" a cada 200 ms
unsigned long ultimaAbertura = 0;
int saturadas = 0;              // amostras na borda (0 ou 1023)

// --- medicao de duracao do pulso (v3.2) ---
// O Arduino e quem melhor enxerga o limiar: ele esta amostrando a
// rede 500 vezes por janela. Medir a duracao aqui evita confiar na
// hora do ESP32 para pulsos de dezenas de milissegundos.
//
// tsInicioPulso vale 0 quando nao ha pulso em andamento.
unsigned long tsInicioPulso = 0;   // millis() do comeco do pulso
unsigned long durUltimoPulso = 0;  // duracao do ultimo pulso (ms)
bool pulsoReportado = false;       // ja mandamos a linha do pulso

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

    Serial.println(F("{\"boot\":\"Sensor OK\",\"versao\":\"3.2\"}"));
    Serial.println(F("# v3.2: ZMPT no A0 — deteccao direta da rua"));
    Serial.println(F("# (metodo Vcc/jack/USB removido)"));
    Serial.println(F("# v3.2: janela 55 ms, debounce 1, reporta PULSO"));
    Serial.print(F("# limiar amplitude="));
    Serial.println(LIMITE_AMPLITUDE);
}

void loop() {
    unsigned long tCiclo = millis();   // para fechar o ciclo em CICLO_MS
    unsigned long janelaUs = 0;
    int subidas = medirJanela(janelaUs);

    // Frequencia: MEDIA de varias janelas, nao a de uma so.
    //
    // Uma janela de 55 ms cobre 3,3 periodos de 60 Hz, entao a
    // contagem de subidas numa unica janela oscila (53..71 Hz em
    // campo). Somando ~20 janelas (~1 s) o erro se dilui e o numero
    // volta a ser estavel — 59.5 Hz, como era com a janela longa.
    //
    // Apenas quando ha sinal: sem rua, o cruzamento e sorteio do
    // ruido e a media nao faria sentido.
    static unsigned long somaSubidas = 0;
    static unsigned long somaUs = 0;
    static byte janelasContadas = 0;
    static long freqMediaCx = -1;   // persiste entre as medições
    long freqCx = -1;               // o que sai nesta linha de diag

    if (amplitude >= LIMITE_AMPLITUDE && subidas > 0 && janelaUs > 0) {
        somaSubidas += subidas;
        somaUs += janelaUs;
        janelasContadas++;
        if (janelasContadas >= 20 && somaUs > 0) {
            freqMediaCx = (long) ((somaSubidas * 10000000UL) / somaUs);
            somaSubidas = 0;
            somaUs = 0;
            janelasContadas = 0;
        }
        freqCx = freqMediaCx;      // disponivel mesmo entre medições
    } else if (amplitude < LIMITE_AMPLITUDE) {
        // Caiu a rua: zera o acumulador para nao misturar uma janela
        // velha de rua com o ruido de depois, e some a frequencia —
        // sem sinal nao ha que se falar em 60 Hz.
        somaSubidas = 0;
        somaUs = 0;
        janelasContadas = 0;
        freqMediaCx = -1;
    }

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

    // --- v3.2: duracao do pulso ---
    // Com o debounce em 1, "bruta" muda ja na primeira janela que
    // enxerga a borda. A resolucao e ~60 ms no pior caso, e melhor
    // quando o pulso cai no meio de uma janela.
    unsigned long agoraMs = millis();
    if (!bruto && tsInicioPulso == 0) {
        tsInicioPulso = agoraMs;      // comecou a cair
        durUltimoPulso = 0;
        pulsoReportado = false;
    } else if (bruto && tsInicioPulso != 0) {
        durUltimoPulso = agoraMs - tsInicioPulso;
        tsInicioPulso = 0;            // terminou
    }

    // Diagnostico (o ESP32 ignora linhas que nao comecam com "{")
    //
    // Só a cada DIAG_MS — ver a nota em DIAG_MS ali em cima. Custa
    // ~65 ms de serial, entao rodar a cada ciclo de 60 ms seria um
    // tiro no pé.
    if (agoraMs - ultimaDiag >= DIAG_MS) {
      ultimaDiag = agoraMs;
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
    }

    // Relatorio de PULSO — a novidade da v3.2.
    //
    // Reporta cada queda e cada volta como um evento proprio, com a
    // duracao medida. O ESP32 e quem decide o que vira alerta; aqui
    // a regra e "nao esconder nada".
    //
    // A linha "pulso" e ANTES da de estado de proposito: quando um
    // pulso curto sobe e desce entre duas janelas de estado, ela e a
    // unica prova de que o pulso existiu.
    if (tsInicioPulso == 0 && durUltimoPulso > 0 && !pulsoReportado) {
        pulsoReportado = true;
        Serial.print(F("{\"pulso\":1,\"dur_ms\":"));
        Serial.print(durUltimoPulso);
        Serial.print(F(",\"amp_min\":"));
        Serial.print(amplitude);      // amplitude na janela do retorno
        Serial.println(F("}"));
        durUltimoPulso = 0;
    }

    // Pulso EM ANDAMENTO: avisa o ESP32 de que a queda nao terminou
    // (sem isso, um blecaute longo pareceria um pulso de duracao zero
    // quando o ESP32 fosse religado no meio dele).
    //
    // A cada PULSO_ABRINDO_MS, nao a cada ciclo: 34 ms a cada 200 ms
    // em vez de 34 ms a cada 60 ms. Um pulso de 300 ms ainda manda 1.
    if (tsInicioPulso != 0 && agoraMs - ultimaAbertura >= PULSO_ABRINDO_MS) {
        ultimaAbertura = agoraMs;
        Serial.print(F("{\"pulso\":1,\"abrindo\":1,\"amp\":"));
        Serial.print(amplitude);
        Serial.println(F("}"));
    }

    // Estado no contrato do gateway. So a partir da 1a confirmacao,
    // igual a v1: evita publicar um estado errado logo no boot.
    //
    // So imprime quando o estado MUDA, ou no heartbeat de 5 s (para
    // o ESP32 nao achar que o Arduino morreu). Em repouso isto custa
    // 34 ms a cada 5 s — em vez de 34 ms a cada 60 ms.
    bool mudou = !estadoEnviado || confirmado != ultimoEstadoEnviado;
    if (confirmadoValido && (mudou || agoraMs - ultimoEstado >= HEARTBEAT_MS)) {
        estadoEnviado = true;
        ultimoEstadoEnviado = confirmado;
        ultimoEstado = agoraMs;
        Serial.print(F("{\"energia\":\""));
        Serial.print(confirmado ? F("presente") : F("ausente"));
        Serial.print(F("\",\"amp\":"));
        Serial.print(amplitude);
        Serial.println(F("}"));
    }

    // LED aceso = QUEDA (sem servico)
    digitalWrite(LED_BUILTIN, confirmado ? HIGH : LOW);

    // Espera o RESTO do ciclo. A janela (~55 ms) ja consumiu quase
    // tudo; o que falta e so o folgado. delay() fixo aqui criaria o
    // buraco que a v3.2 existe para eliminar.
    unsigned long gasto = millis() - tCiclo;
    if (gasto < CICLO_MS) delay(CICLO_MS - gasto);
}
