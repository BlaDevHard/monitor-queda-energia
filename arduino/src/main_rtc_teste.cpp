/*
 * SONDA INTELIGENTE — localiza a fiação do DS1302 sozinha
 * =======================================================
 * O QUE FAZ (em ordem):
 *   1) FASE 1 — liga pull-up interno em todos os pinos livres e le
 *      o nivel de cada um. Fio preso em GND ou no módulo (que tem
 *      pull-down interno de 40k) aparece como 0; pino sem fio, 1.
 *   2) FASE 2 — testa combinacoes (RST, CLK, DAT) sobre os pinos
 *      candidatos, e se falhar sobre todos, ate o DS1302 devolver o
 *      padrao gravado na RAM dele. CONFIRMA com DOIS padroes
 *      (0xA5 e 0x5A) — fio curto nao devolve os dois.
 *   3) Com os pinos achados: imprime diagnostico completo (RAM,
 *      registrador de carga/WP, hora, epoch) e aceita #SETTS.
 *   4) Loop: diagnostico por ciclo + linha JSON IDENTICA a da v2.1.
 *
 * O GATEWAY NAO MEXE: a linha {"energia":"presente","amp":1023} e a
 * mesma da v2.1 — o ESP32 nao ve transicao de estado nem alerta.
 * Linhas de diagnostico comecam com "#" ou "VCC=" (nao com "{"),
 * entao o ESP32 ignora.
 *
 * EXCLUSOES DE PROPOSITO na varredura:
 *   D0/D1 = serial (D1 -> divisor -> GPIO16 do ESP32)
 *   D13   = LED da placa
 *   A0    = saida do ZMPT (conectado no sistema)
 *
 * CR2032 NAO E NECESSARIA p/ teste — o chip roda nos 5V. A bateria
 * so segura a hora quando a energia cai (cenario real de rua).
 *
 * Gravar: pio run -e rtc_teste -t upload
 */

#include <Arduino.h>
#include "ds1302.h"

#define CICLO_MS 500

// Pinos varridos: D2..D12 e A1..A5 (como digitais).
static const uint8_t PINS[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 17, 18, 19};
#define NPINS (uint8_t)(sizeof(PINS) / sizeof(PINS[0]))

// Fiação encontrada pela varredura (fixada depois do achado).
static uint8_t pinRst = 0, pinClk = 0, pinDat = 0;
static bool fioAchado = false;
static bool rtcPronto = false;   // rodando + hora valida
static long rtcTs = -1;          // epoch lido (-1 = sem hora boa)

// ---------------- Vcc pelo bandgap interno --------------------------
// Diagnostico de calibracao (VCC_LIMITE_MV) — mesma tecnica da v3.
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
    return (unsigned int) ((BANDGAP_MV * 1024UL) / media);
}

// ---------------- utilidades de impressao ---------------------------
static void imprimirNome(uint8_t p) {
    if (p >= 15) {              // A1..A5 = digitais 15..19
        Serial.print('A');
        Serial.print(p - 14);
    } else {
        Serial.print('D');
        Serial.print(p);
    }
}

// ---------------- FASE 2: teste funcional --------------------------
// Combinacao vale se o chip devolver OS DOIS padroes escritos na
// RAM dele. Resposta 0x00/0xFF/0x55 (fio solto, em curto, CLK/DAT
// ligados entre si) falha nos dois e e descartada.
static bool testarCombinacao(uint8_t rst, uint8_t clk, uint8_t dat) {
    rtcIniciar(rst, clk, dat);   // WP=0 + carga desligada
    if (!rtcTesteRamValor(0xA5)) return false;
    if (!rtcTesteRamValor(0x5A)) return false;
    return true;
}

static bool tentarLista(const uint8_t* p, uint8_t n, uint8_t& rst, uint8_t& clk, uint8_t& dat) {
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            if (j == i) continue;
            for (uint8_t k = 0; k < n; k++) {
                if (k == i || k == j) continue;
                if (testarCombinacao(p[i], p[j], p[k])) {
                    rst = p[i]; clk = p[j]; dat = p[k];
                    return true;
                }
            }
        }
    }
    return false;
}

// ---------------- FASE 1 + FASE 2 -----------------------------------
static void localizarFio() {
    static uint8_t baixos[NPINS];
    uint8_t nBaixos = 0;

    // FASE 1: pull-up em tudo e le o nivel de cada pino.
    for (uint8_t i = 0; i < NPINS; i++) pinMode(PINS[i], INPUT_PULLUP);
    delayMicroseconds(20);

    Serial.println(F("# FASE 1 — nivel de cada pino livre (pull-up ligado):"));
    Serial.print(F("#   "));
    for (uint8_t i = 0; i < NPINS; i++) {
        uint8_t v = digitalRead(PINS[i]);
        if (v == 0) baixos[nBaixos++] = PINS[i];
        imprimirNome(PINS[i]);
        Serial.print('=');
        Serial.print(v);
        Serial.print(' ');
    }
    Serial.println();
    Serial.print(F("#   candidatos (nivel 0): "));
    if (nBaixos == 0) {
        Serial.println(F("nenhum"));
    } else {
        for (uint8_t i = 0; i < nBaixos; i++) { imprimirNome(baixos[i]); Serial.print(' '); }
        Serial.println();
    }

    // FASE 2: procura a combinacao que responde.
    Serial.println(F("# FASE 2 — procurando o DS1302..."));
    fioAchado = tentarLista(baixos, nBaixos, pinRst, pinClk, pinDat);

    if (!fioAchado && nBaixos < NPINS) {
        Serial.println(F("#   falhou nos candidatos, varrendo TODOS os pinos livres..."));
        fioAchado = tentarLista(PINS, NPINS, pinRst, pinClk, pinDat);
    }

    if (fioAchado) {
        Serial.print(F("# FIAÇÃO ENCONTRADA: RST="));
        imprimirNome(pinRst);
        Serial.print(F("  CLK="));
        imprimirNome(pinClk);
        Serial.print(F("  DAT="));
        imprimirNome(pinDat);
        Serial.println();
        Serial.println(F("#   (use essa tabela no firmware v3.1)"));
    } else {
        Serial.println(F("# NENHUMA combinacao respondeu."));
        Serial.println(F("# O modulo nao esta ativo no barramento. Confira:"));
        Serial.println(F("#   1) VCC do modulo nos 5V e GND no GND (alimentacao!)"));
        Serial.println(F("#   2) os 3 fios do modulo — a ordem fisica dos pinos"));
        Serial.println(F("#      (RST/CLK/DAT) varia de um modulo para outro"));
        Serial.println(F("#   3) CR2032 nao e necessaria p/ este teste"));
    }
}

// #MTEST — caracteriza o registrador do MES (endereco 5), que
// recusa o bit da dezena no acesso single. Tenta valores sonde
// e depois o burst (relogio inteiro de uma vez).
static void testeMes() {
    const uint8_t tentativas[] = {0x12, 0x0A, 0x20, 0x10, 0x09};
    Serial.println(F("# MTEST single-writes no reg5:"));
    for (uint8_t i = 0; i < sizeof(tentativas); i++) {
        rtcEscreverRegistrador(5, tentativas[i]);
        Serial.print(F("#   reg5 <- 0x"));
        Serial.print(tentativas[i], HEX);
        Serial.print(F("   le=0x"));
        Serial.println(rtcLerRegistrador(5), HEX);
    }

    // Burst write: sec,min,hour,dow,day,month,year,ctrl (tudo BCD
    // valido: 10:10:10, dia 10, mes 0x10, ano 2026, WP=0).
    const uint8_t regs[8] = {0x10, 0x10, 0x10, 0x05, 0x10, 0x10, 0x26, 0x00};
    rtcBurstGravar(regs);

    uint8_t le[8];
    rtcBurstLer(le);
    Serial.print(F("# MTEST burst-read:   "));
    for (uint8_t i = 0; i < 8; i++) {
        Serial.print('R'); Serial.print(i); Serial.print("=0x");
        Serial.print(le[i], HEX);
        Serial.print(' ');
    }
    Serial.println();

    Serial.print(F("# MTEST single-read:  "));
    for (uint8_t i = 0; i < 7; i++) {
        Serial.print('R'); Serial.print(i); Serial.print("=0x");
        Serial.print(rtcLerRegistrador(i), HEX);
        Serial.print(' ');
    }
    Serial.println();
    Serial.println(F("# (o #SETTS a seguir regrava a hora certa)"));
}

// ---------------- diagnostico com a fiação ja achada ----------------
// #WTEST — teste de escrita registrador por registrador. Distingue
// "endereco errado" de "valor que nao grava" de "leitura errada".
static void testeEscrita() {
    // Fase 1: valor distinto (e valido) por registrador.
    const uint8_t vals[7] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    Serial.println(F("# WTEST fase 1: gravando 0x01..0x07 nos regs 0..6"));
    for (uint8_t a = 0; a <= 6; a++) rtcEscreverRegistrador(a, vals[a]);
    Serial.print(F("#   leitura: "));
    for (uint8_t a = 0; a <= 6; a++) {
        Serial.print('R'); Serial.print(a); Serial.print("=0x");
        Serial.print(rtcLerRegistrador(a), HEX);
        Serial.print(' ');
    }
    Serial.println();
    Serial.println(F("#   (esperado: cada Rn igual ao seu valor gravado)"));

    // Fase 2: 0x10 (o valor do MES que falhou) em todos.
    Serial.println(F("# WTEST fase 2: gravando 0x10 em todos os regs 0..6"));
    for (uint8_t a = 0; a <= 6; a++) rtcEscreverRegistrador(a, 0x10);
    Serial.print(F("#   leitura: "));
    for (uint8_t a = 0; a <= 6; a++) {
        Serial.print('R'); Serial.print(a); Serial.print("=0x");
        Serial.print(rtcLerRegistrador(a), HEX);
        Serial.print(' ');
    }
    Serial.println();

    // Fase 3: controle (WP) e carga (trickle).
    rtcEscreverRegistrador(7, 0x00);
    rtcEscreverRegistrador(8, 0x00);
    Serial.print(F("# WTEST fase 3: ctrl=0x"));
    Serial.print(rtcLerRegistrador(7), HEX);
    Serial.print(F(" trk=0x"));
    Serial.println(rtcLerRegistrador(8), HEX);
    Serial.println(F("# (o #SETTS a seguir regrava a hora certa)"));
}

static void diagnosticarRtc() {
    uint8_t datOcioso = rtcDatOcioso();
    Serial.print(F("# fio do DAT (ocioso, pull-up): "));
    Serial.println(datOcioso ? F("ALTO — fio solto/no lugar")
                             : F("BAIXO — forçado em nivel 0"));

    uint8_t ramLido = 0;
    bool ramOk = rtcTesteRam(&ramLido);
    Serial.print(F("# RAM: escrevi 0xA5, li 0x"));
    Serial.print(ramLido, HEX);
    Serial.println(ramOk ? F(" -> OK (modulo respondendo)")
                         : F(" -> FALHOU (modulo ausente)"));

    uint8_t ctrl = rtcLerRegistrador(7);
    uint8_t trk = rtcLerRegistrador(8);
    Serial.print(F("# controle(WP)="));
    Serial.print(ctrl & 0x80 ? 1 : 0);
    Serial.print(F("  carga(trickle)=0x"));
    Serial.print(trk, HEX);
    // So o padrao TCS=1010 liga o carregador; qualquer outro valor
    // deixa a CR2032 em seguranca.
    Serial.println((trk & 0xF0) == 0xA0 ? F(" (CARGA ATIVA! corrigir antes do CR2032)")
                                        : F(" (carga desligada — seguro p/ CR2032)"));

    Ds1302Hora h;
    rtcPronto = rtcLer(h);
    rtcTs = rtcEpoch(h);

    if (!ramOk) {
        return;
    } else if (h.rodando && !h.valido) {
        Serial.print(F("# RTC responde mas HORA INVALIDA: ano="));
        Serial.print(h.ano);
        Serial.print(F(" mes=")); Serial.print(h.mes);
        Serial.print(F(" dia=")); Serial.print(h.dia);
        Serial.print(F("  hora=")); Serial.print(h.hora);
        Serial.print(':'); Serial.print(h.minuto);
        Serial.print(':'); Serial.println(h.segundo);
        Serial.println(F("# Ajuste com: #SETTS=<epoch>  (epoch = date +%s)"));
    } else if (!h.rodando) {
        Serial.println(F("# RTC PARADO (CH=1, oscilador desligado)"));
        Serial.println(F("# Ajuste com: #SETTS=<epoch>  (epoch = date +%s)"));
    } else {
        Serial.print(F("# RTC OK — epoch="));
        Serial.println(rtcTs);
        Serial.println(F("# Confira: date -u -d @epoch  deve bater com a hora"));
    }
}

// ---------------- comando #SETTS (vem do PC) ------------------------
static char bufSerial[32];
static uint8_t bufLen = 0;

static void processarComando() {
    bufSerial[bufLen] = '\0';
    if (strncmp(bufSerial, "#SETTS=", 7) == 0) {
        long ep = atol(bufSerial + 7);
        Ds1302Hora h;
        if (!rtcDeEpoch(ep, h)) {
            Serial.print(F("# SETTS REJEITADO: epoch "));
            Serial.print(ep);
            Serial.println(F(" fora do intervalo valido (0..2147483647)"));
        } else if (!rtcGravar(h)) {
            Serial.println(F("# SETTS FALHOU: releitura nao bateu com a escrita"));
            Serial.println(F("#   (confira os fios do DS1302)"));
        } else {
            Serial.print(F("# SETTS OK: epoch="));
            Serial.print(ep);
            Ds1302Hora r;
            rtcLer(r);
            Serial.print(F("  releitura="));
            Serial.println(rtcEpoch(r));
        }
        diagnosticarRtc();   // re-imprime o estado completo
    } else if (strncmp(bufSerial, "#WTEST", 6) == 0) {
        testeEscrita();
    } else if (strncmp(bufSerial, "#MTEST", 6) == 0) {
        testeMes();
    } else if (strncmp(bufSerial, "#RTC", 4) == 0) {
        diagnosticarRtc();
    } else if (bufLen > 0) {
        Serial.print(F("# comando desconhecido: "));
        Serial.println(bufSerial);
    }
    bufLen = 0;
}

static void lerSerial() {
    while (Serial.available() > 0) {
        char c = (char) Serial.read();
        if (c == '\n' || c == '\r') {
            if (bufLen > 0) processarComando();
            bufLen = 0;
        } else if (bufLen < sizeof(bufSerial) - 1) {
            bufSerial[bufLen++] = c;
        }
    }
}

// ---------------- setup / loop --------------------------------------
void setup() {
    Serial.begin(9600);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);   // apagado = servico ativo (v2.1)
    delay(100);

    Serial.println(F("# SONDA RTC DS1302 (temporaria) — varredura de fiação"));
    localizarFio();
    if (fioAchado) diagnosticarRtc();
    Serial.println(F("# Aceita: #SETTS=<epoch>   #RTC"));
    Serial.println(F("# Enviando estado pro gateway (linha da v2.1, identica)"));
}

void loop() {
    lerSerial();

    // Diagnostico por ciclo (linhas sem "{" sao ignoradas pelo ESP32).
    Serial.print(F("VCC="));
    Serial.print(lerVcc_mV());
    Serial.print(F(" mV  "));

    if (!fioAchado) {
        Serial.println(F("RTC=ausente (nenhum pino respondeu)"));
    } else {
        uint8_t ram = 0;
        bool ramOk = rtcTesteRam(&ram);
        if (!ramOk) {
            Serial.println(F("RTC=ausente (RAM FALHOU — fio soltou?)"));
            fioAchado = false;
        } else {
            Ds1302Hora h;
            bool ok = rtcLer(h);
            rtcPronto = ok;
            rtcTs = rtcEpoch(h);
            if (ok) {
                Serial.print(F("RTC=ok ts="));
                Serial.println(rtcTs);
            } else if (h.rodando) {
                Serial.println(F("RTC=invalido (hora fora de faixa)"));
            } else {
                Serial.print(F("RTC=parado ts="));
                Serial.println(rtcTs);
            }
        }
    }

    // Linha de estado IDENTICA a v2.1 — o gateway ja a trata hoje.
    Serial.println(F("{\"energia\":\"presente\",\"amp\":1023}"));

    delay(CICLO_MS);
}
