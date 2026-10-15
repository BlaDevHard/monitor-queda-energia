/*
 * DRIVER DO RTC DS1302 — implementacao (3 fios, bit-bang)
 * Ver ds1302.h para a visao geral, convencoes e limites.
 */

#include "ds1302.h"

// ---- pinos (fixos apos rtcIniciar) ----
static uint8_t _rst = 8;
static uint8_t _clk = 6;
static uint8_t _dat = 7;

// ==================================================================
//  CONVERSOES
// ==================================================================
static inline uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Dias desde 1970-01-01 (algoritmo de Hinnant — vale para 1970..2038+)
static long diasDesdeCivil(long a, unsigned m, unsigned d) {
    a -= m <= 2;
    const long e = (a / 400) - (a / 100) + (a / 4);
    const long p = (153L * (long)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (long)d - 1;
    return 365 * a + e + p - 719468;
}

// Inverso: epoch -> civil (data). z = dias desde 1970-01-01.
static void civilDesdeDias(long z, long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned long doe = (unsigned long)(z - era * 146097);      // [0..146096]
    const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long yy = (long)yoe + era * 400;
    const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0..364]
    const unsigned long mp = (5 * doy + 2) / 153;                      // [0..11]
    d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);                      // [1..31]
    m = (mp < 10) ? (unsigned)(mp + 3) : (unsigned)(mp - 9);           // [1..12]
    y = yy + (m <= 2);
}

// ==================================================================
//  BAIXO NIVEL — 3 fios
// ==================================================================
// Um "byte" do DS1302 vai LSB primeiro. Nas ESCRITAS o chip amostra
// na borda de subida; nas LEITAS ele solta o bit na borda de descida
// (o primeiro bit aparece na descida logo apos o ultimo bit do
// comando) — por isso o lerByte le o pino ANTES de pulsar o clock.
// ==================================================================
static void escreverByte(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(_dat, (b & 1) ? HIGH : LOW);
        digitalWrite(_clk, HIGH);   // subida: chip amostra
        digitalWrite(_clk, LOW);    // descida: proximo bit
        b >>= 1;
    }
}

static uint8_t lerByte() {
    uint8_t v = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (digitalRead(_dat)) v |= (1 << i);  // bit ja disponivel (descida anterior)
        digitalWrite(_clk, HIGH);               // subida: chip prepara o proximo
        digitalWrite(_clk, LOW);                // descida: chip solta o proximo bit
    }
    return v;
}

// Comeca uma transacao: RST sobe com CLK em baixo (exigencia do chip).
static void iniciar() {
    digitalWrite(_clk, LOW);
    digitalWrite(_rst, HIGH);
    delayMicroseconds(4);
}

static void encerrar() {
    digitalWrite(_rst, LOW);
    delayMicroseconds(4);
}

// Leitura de um registrador: comando (rw=1) e depois o byte de dados.
static uint8_t lerReg(uint8_t cmd) {
    pinMode(_dat, OUTPUT);
    iniciar();
    escreverByte(cmd);          // bit0 do comando = 1 (leitura)
    pinMode(_dat, INPUT_PULLUP); // chip assume o barramento
    uint8_t v = lerByte();
    encerrar();
    return v;
}

// Escrita de um registrador: comando (rw=0) e depois o dado.
static void escreverReg(uint8_t cmd, uint8_t val) {
    pinMode(_dat, OUTPUT);
    iniciar();
    escreverByte(cmd);          // bit0 do comando = 0 (escrita)
    escreverByte(val);          // chip amostra na subida
    encerrar();
}

// ==================================================================
//  API PUBLICA
// ==================================================================
// Comandos: [1][RAM/rel][A4..A0][R/W]  — bit7 fixo 1, bit0 = R/W
#define CMD_RELOGIO(addr, rw) (uint8_t)(0x80 | (((addr) & 0x1F) << 1) | ((rw) & 1))
#define CMD_RAM(addr, rw)     (uint8_t)(0x80 | 0x40 | (((addr) & 0x1F) << 1) | ((rw) & 1))

// Registradores: 0=seg 1=min 2=hora 3=diaSem 4=dia 5=mes 6=ano
//                7=controle (bit7=WP)  8=trickle/carga (TCS[7:4])
#define REG_SEG      0
#define REG_CONTROLE 7
#define REG_TRICKLE  8

void rtcIniciar(uint8_t pinoRst, uint8_t pinoClk, uint8_t pinoDat) {
    _rst = pinoRst;
    _clk = pinoClk;
    _dat = pinoDat;

    // RST e CLK sao de Saida com nivel BAIXO desde o primeiro momento
    // (o chip exige RST=0 e SCLK=0 ate o VCC estabilizar).
    digitalWrite(_rst, LOW);
    digitalWrite(_clk, LOW);
    digitalWrite(_dat, LOW);
    pinMode(_rst, OUTPUT);
    pinMode(_clk, OUTPUT);
    pinMode(_dat, OUTPUT);

    // CR2032 comum: escrita liberada (WP=0) e CARREGADOR DESLIGADO.
    // O registrador de carga so aceita 0x00 se o WP estiver em 0,
    // entao o controle vem primeiro.
    escreverReg(CMD_RELOGIO(REG_CONTROLE, 0), 0x00);  // WP=0
    escreverReg(CMD_RELOGIO(REG_TRICKLE, 0), 0x00);   // trickle OFF (0000)
}

uint8_t rtcLerRegistrador(uint8_t addr) {
    return lerReg(CMD_RELOGIO(addr, 1));
}

void rtcEscreverRegistrador(uint8_t addr, uint8_t val) {
    escreverReg(CMD_RELOGIO(addr, 0), val);
}

uint8_t rtcDatOcioso() {
    // RST=0 deixa o chip em alta impedancia no DAT (barramento
    // "solto"). Com pull-up interno ligado, o que aparecer e o
    // estado do fio: alto = ok/solto, baixo = forçado (curto).
    digitalWrite(_rst, LOW);
    digitalWrite(_clk, LOW);
    pinMode(_dat, INPUT_PULLUP);
    delayMicroseconds(10);
    return digitalRead(_dat);
}

bool rtcTesteRamValor(uint8_t valor, uint8_t* lido) {
    // Escreve um padrao na RAM[0] e le de volta. Se o modulo nao
    // estiver respondendo (fio solto, pino trocado), o byte nao volta.
    escreverReg(CMD_RAM(0, 0), valor);
    uint8_t v = lerReg(CMD_RAM(0, 1));
    if (lido) *lido = v;
    return v == valor;
}

bool rtcTesteRam(uint8_t* lido) {
    return rtcTesteRamValor(0xA5, lido);
}

void rtcBurstLer(uint8_t regs[8]) {
    pinMode(_dat, OUTPUT);
    iniciar();
    escreverByte(0xBF);              // burst read do relogio (0..7)
    pinMode(_dat, INPUT_PULLUP);
    for (uint8_t i = 0; i < 8; i++) regs[i] = lerByte();
    encerrar();
    pinMode(_dat, OUTPUT);
    digitalWrite(_dat, LOW);
}

void rtcBurstGravar(const uint8_t regs[8]) {
    pinMode(_dat, OUTPUT);
    iniciar();
    escreverByte(0xBE);              // burst write do relogio (0..7)
    for (uint8_t i = 0; i < 8; i++) escreverByte(regs[i]);
    encerrar();
}

bool rtcLer(Ds1302Hora& h) {
    uint8_t seg = rtcLerRegistrador(REG_SEG);
    h.rodando = !(seg & 0x80);          // CH (clock halt) = 0 -> correndo
    h.segundo = bcd2bin(seg & 0x7F);

    h.minuto = bcd2bin(rtcLerRegistrador(1) & 0x7F);

    uint8_t horaReg = rtcLerRegistrador(2);
    if (horaReg & 0x80) {
        // Modo 12h (bit7=1): bit5 = PM. Convertemos para 24h.
        h.hora24 = false;
        uint8_t h12 = bcd2bin(horaReg & 0x1F);
        bool pm = horaReg & 0x20;
        h.hora = (h12 % 12) + (pm ? 12 : 0);
    } else {
        h.hora24 = true;
        h.hora = bcd2bin(horaReg & 0x3F);
    }

    h.diaSemana = bcd2bin(rtcLerRegistrador(3) & 0x3F);
    h.dia = bcd2bin(rtcLerRegistrador(4) & 0x3F);
    h.mes = bcd2bin(rtcLerRegistrador(5) & 0x1F);
    h.ano = 2000 + bcd2bin(rtcLerRegistrador(6));

    // Mes que o chip nao relogue (CH=1), a hora "congelada" ainda e
    // valida como leitura — mas hora PARADA nao serve pro protocolo.
    h.valido =
        h.segundo <= 59 && h.minuto <= 59 && h.hora <= 23 &&
        h.dia >= 1 && h.dia <= 31 && h.mes >= 1 && h.mes <= 12 &&
        h.ano >= 2000 && h.ano <= 2038 && h.diaSemana >= 1 && h.diaSemana <= 7;

    return h.rodando && h.valido;
}

bool rtcGravar(const Ds1302Hora& h) {
    if (!(h.segundo <= 59 && h.minuto <= 59 && h.hora <= 23 &&
          h.dia >= 1 && h.dia <= 31 && h.mes >= 1 && h.mes <= 12 &&
          h.ano >= 2000 && h.ano <= 2038)) {
        return false;
    }

    // Seguranca repetida aqui (mesmo ja feito no iniciar): WP=0 e
    // carga desligada — a CR2032 nao pode levar corrente.
    escreverReg(CMD_RELOGIO(REG_CONTROLE, 0), 0x00);
    escreverReg(CMD_RELOGIO(REG_TRICKLE, 0), 0x00);

    uint8_t dow = h.diaSemana >= 1 && h.diaSemana <= 7 ? h.diaSemana : 1;

    escreverReg(CMD_RELOGIO(REG_SEG, 0), bin2bcd(h.segundo));  // bit7=0: inicia o relogio
    escreverReg(CMD_RELOGIO(1, 0), bin2bcd(h.minuto));
    escreverReg(CMD_RELOGIO(2, 0), bin2bcd(h.hora));           // bit7=0: forca modo 24h
    escreverReg(CMD_RELOGIO(3, 0), bin2bcd(dow));
    escreverReg(CMD_RELOGIO(4, 0), bin2bcd(h.dia));
    escreverReg(CMD_RELOGIO(5, 0), bin2bcd(h.mes));
    escreverReg(CMD_RELOGIO(6, 0), bin2bcd((uint8_t)(h.ano - 2000)));

    // Conferencia: rele e compara (menos o segundo, que ja pode ter
    // andado durante a gravacao).
    Ds1302Hora r;
    if (!rtcLer(r)) return false;
    return r.ano == h.ano && r.mes == h.mes && r.dia == h.dia &&
           r.hora == h.hora && r.minuto == h.minuto;
}

long rtcEpoch(const Ds1302Hora& h) {
    if (!(h.ano >= 2000 && h.ano <= 2038 && h.mes >= 1 && h.mes <= 12 &&
          h.dia >= 1 && h.dia <= 31 && h.hora <= 23 && h.minuto <= 59 &&
          h.segundo <= 59)) {
        return -1;
    }
    return diasDesdeCivil(h.ano, h.mes, h.dia) * 86400L +
           h.hora * 3600L + h.minuto * 60L + h.segundo;
}

bool rtcDeEpoch(long epoch, Ds1302Hora& h) {
    // 0..2147483647: epoch positivo dentro do int32 do AVR (ate 2038).
    if (epoch <= 0 || epoch > 2147483647L) return false;

    long dias = epoch / 86400L;
    long segs = epoch % 86400L;

    long y;
    unsigned m, d;
    civilDesdeDias(dias, y, m, d);

    h.ano = (uint16_t)y;
    h.mes = (uint8_t)m;
    h.dia = (uint8_t)d;
    h.hora = (uint8_t)(segs / 3600L);
    h.minuto = (uint8_t)((segs % 3600L) / 60L);
    h.segundo = (uint8_t)(segs % 60L);
    // Convencao do DS1302: 1=domingo..7=sabado. 1970-01-01 = quinta (5).
    h.diaSemana = (uint8_t)(((dias + 4) % 7) + 1);
    h.rodando = true;
    h.hora24 = true;
    h.valido = true;
    return true;
}
