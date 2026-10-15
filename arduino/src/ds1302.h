#pragma once
#include <Arduino.h>

/*
 * DRIVER DO RTC DS1302 — interface de 3 fios (RST, CLK, DAT)
 * ==========================================================
 * NAO e I2C: RST (CE), CLK (SCLK) e DAT (I/O) sao bit-bangados.
 * Escrevido a mao para o projeto (sem biblioteca externa) porque
 * precisamos de duas coisas que biblioteca pronta costuma esconder:
 *
 *   1) DESLIGAR o trickle charge (carregador de bateria). O modulo
 *      vem com CR2032 comum — NAO recarregavel. Carregar uma CR2032
 *      e risco de vazamento. rtcIniciar() escreve 0x00 no
 *      registrador 8 (trickle) e 0x00 no 7 (WP, escrita liberada).
 *
 *   2) Controle total do barramento (o Uno tem so 5 V; o DS1302
 *      tambem — combinacao direta, sem divisor).
 *
 * CONVENCAO DE HORA: o registrador guarda UTC puro (sem fuso).
 * O epoch que sai daqui e direto (rtcEpoch); formatar em GMT-3 e
 * papel de quem exibe (ESP32/relogio.cpp).
 *
 * LIMITE: epoch em `long` do AVR = 32 bits -> vale ate 19/01/2038
 * (o mesmo limite de todo o resto do sistema).
 *
 * TIMING (datasheet): dado de ENTRADA e amostrado na borda de
 * subida do CLK; dado de SAIDA e colocado na borda de DESCIDA
 * (primeiro bit no primeiro 0 do CLK depois do ultimo bit do
 * comando). Por isso o lerByte le ANTES de pulsar o clock.
 */

struct Ds1302Hora {
    uint16_t ano;       // 2000..2038
    uint8_t mes;        // 1..12
    uint8_t dia;        // 1..31
    uint8_t diaSemana;  // 1..7 (convencao do chip)
    uint8_t hora;       // 0..23
    uint8_t minuto;     // 0..59
    uint8_t segundo;    // 0..59
    bool rodando;       // CH=0: oscilador ligado
    bool valido;        // todos os campos dentro das faixas
    bool hora24;        // formato original encontrado no chip
};

// Pinagem no Uno: RST=D8, CLK=D6, DAT=D7 (D1=TX pro ESP32, D13=LED).
// Alem de configurar os pinos, ja desliga WP e trickle charge.
void rtcIniciar(uint8_t pinoRst, uint8_t pinoClk, uint8_t pinoDat);

// Leitura crua de um registrador (0..8 = relogio+controle+carga).
// So para diagnostico/impressao.
uint8_t rtcLerRegistrador(uint8_t addr);

// Escrita crua de um registrador (0..8). So para diagnostico.
void rtcEscreverRegistrador(uint8_t addr, uint8_t val);

// Diagnostico de fiação: le o DAT com o barramento OCIOSO (RST=0,
// chip nao selecionado, pull-up interno ligado). O nivel lido vem
// do FIO, nao do chip. 1 = linha em alto (fio solto/no lugar);
// 0 = linha em baixo mesmo com pull-up -> provavel curto no GND
// ou o fio do DAT em um pino errado.
uint8_t rtcDatOcioso();

// Teste de barramento: grava um padrao no RAM[0] e le de volta.
// true = modulo respondendo. Se quiser, devolve o byte lido.
bool rtcTesteRamValor(uint8_t valor, uint8_t* lido = nullptr);

// Atalho: teste com o padrao classico 0xA5.
bool rtcTesteRam(uint8_t* lido = nullptr);

// Le a hora do chip. Retorna true se rodando E dentro das faixas.
// Mesmo retornando false, `h` vem preenchido (para diagnostico).
bool rtcLer(Ds1302Hora& h);

// Burst: os 8 registradores do relogio (0..7) de uma vez (0xBF/0xBE).
// Algum modulo/clones tem defeito no acesso SINGLE ao registrador do
// mes (bit da dezena nao grava); burst e o caminho da datasheet.
void rtcBurstLer(uint8_t regs[8]);
void rtcBurstGravar(const uint8_t regs[8]);

// Ajusta a hora (forca formato 24h, desliga WP e trickle de novo)
// e confere a gravacao por releitura. false = falhou.
bool rtcGravar(const Ds1302Hora& h);

// civil (UTC) -> epoch. -1 se os campos estiverem fora de faixa.
long rtcEpoch(const Ds1302Hora& h);

// epoch -> civil (UTC). false se o epoch estiver fora do intervalo
// valido (0 .. 2147483647, limite de 2038 do AVR).
bool rtcDeEpoch(long epoch, Ds1302Hora& h);
