/*
 * NUCLEO COMPARTILHADO — Monitor de Quedas de Energia (ESP32)
 * v3.1: deteccao DIRETA da rua — o Arduino le o ZMPT101B no A0.
 * (A v3.0 media a propria Vcc e inferia a troca tomada -> power
 * bank; foi removida junto com "fonte"/"vcc".)
 * Ver monitor_core.h para a visao geral.
 */

#include "monitor_core.h"
#include "relogio.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>

#include <Update.h>        // OTA 1.3 — grava a nova imagem
#include <esp_ota_ops.h>   // OTA 1.5 — rollback + slot ativo

#include "config.h"   // WiFi, tokens, MQTT, pinos (nao versionado)

// ---- padroes (para compilar com um config.h antigo) ----
#ifndef FILA_MAX
#define FILA_MAX 8          // mensagens guardadas com a rede fora
#endif
#ifndef CHECKPOINT_MS
#define CHECKPOINT_MS 60000UL   // periodicidade do checkpoint na flash
#endif
#ifndef AVISAR_SEM_SENSOR
#define AVISAR_SEM_SENSOR 1
#endif
#ifndef SENSOR_SILENCIO_MS
#define SENSOR_SILENCIO_MS 45000UL   // 45 s sem linha do Arduino
#endif
#ifndef HORARIA_MS
#define HORARIA_MS 3600000UL   // relatorio horario: 60 min
#endif
#ifndef K_VRMS
#define K_VRMS 0.2987F   // regressao de 5 pontos, 18/10/2026 (ver config.example.h)
#endif
#ifndef V_NOM
#define V_NOM 127        // tensao nominal da rede (PRODIST módulo 8)
#endif
#ifndef MODO_MUDO
#define MODO_MUDO 0   // 1 = bancada: nada sai p/ Discord, Pushbullet ou MQTT
#endif

// ---- OTA: atualizacao do firmware pela rede (subtasks 1.2..1.6) ----
//
// Sem OTA_USUARIO e OTA_SENHA definidos no config.h, o endpoint
// /update NEM E REGISTRADO. Um config.h antigo ou incompleto nao pode
// ganhar por acidente uma porta de regravacao aberta na rede — e se
// alguem marcar OTA_ATIVO=1 sem credencial, isso vira ERRO DE
// COMPILACAO (ver #error abaixo), nao uma senha fraca de fabrica.
#if defined(OTA_USUARIO) && defined(OTA_SENHA)
#ifndef OTA_ATIVO
#define OTA_ATIVO 1
#endif
#else
#ifndef OTA_ATIVO
#define OTA_ATIVO 0
#endif
#endif

#if OTA_ATIVO && !(defined(OTA_USUARIO) && defined(OTA_SENHA))
#error "OTA_ATIVO=1 exige OTA_USUARIO e OTA_SENHA no config.h"
#endif

// Vida minima antes de confirmar a imagem nova (ver esp_ota_ops.h):
// enquanto a imagem esta em ESP_OTA_IMG_PENDING_VERIFY, qualquer
// reset antes da confirmacao volta para a versao anterior. Tempo de
// mais nao protege nada e atrasa a confirmacao; tempo de menos
// confirma cedo demais. 30 s + sensor falando (em coreLoop).
#ifndef OTA_CONFIRMA_MS
#define OTA_CONFIRMA_MS 30000UL
#endif

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ---- estado global ----
bool emQueda = false;
bool estadoInicialEnviado = false;
String estadoAtual = "?";
int ampAtual = -1;
bool wifiOk = false;
bool sinalSaturado = false;

// ---- interno ----
static String nomeFw = "esp32";
static unsigned long tsQueda = 0;          // millis() do inicio da queda
static time_t tsQuedaEpoch = 0;            // epoch do inicio da queda
static unsigned long tsMudanca = 0;        // millis() da ultima mudanca
static unsigned long durUltimaQueda = 0;   // duracao da ultima queda (ms)
static String horaMudanca = "-";           // hora da ultima mudanca

// watchdog do sensor (ver sensorPulsou / coreLoop)
static unsigned long ultimaLeituraSensor = 0;
static bool sensorSilencioso = false;

// ---- registro de pulsos curtos (v3.2) ----
// Declaracoes adiantadas: estas funcoes estao definidas mais abaixo
// e o bloco de pulsos precisa delas (o relatorio sai formatado igual
// as outras mensagens).
static String identificacao();
static String agoraStr();
static String notaHora(time_t t);
static void publicar(const String& titulo, const String& corpo, int cor);
// Definida junto da logica de queda (mais abaixo); o relatorio
// horario, que vem antes, ja a referencia.
static float vrmsAtual();

// O Arduino reporta CADA pulso (queda e volta) com a duracao que
// ele mesmo mediu. Aqui juntamos varios pulsos num relatorio so —
// e o caso real que motiva: um blecaute de 5 s pode virar 3 quedas
// e 3 retornos, e mandar 6 mensagens de Discord nao ajuda ninguem.
#define PULSO_MAX 64              // pulsos guardados antes de forcar o envio
#define JANELA_PULSO_MS 5000      // 5 s: agrupa pulsos desta janela
#define LIMITE_RELATORIO_MS 1000  // abaixo disso e "flicker", so entra no relatorio

static unsigned long pulsoInicio = 0;   // inicio do pulso em curso (0 = nenhum)
static int pulsoAmpMin = -1;            // amplitude minima vista no pulso
static unsigned long pulsoJanelaFim = 0;  // fim da janela de agrupamento
static bool pulsoRelatorioPendente = false;
static unsigned long pulsoTotal = 0;      // contador desde o boot

// --- DECISAO: mensagem avulsada ou relatorio? ---
//
// O problema de principio: no instante em que a rua cai, o ESP32 ainda
// NAO SABE se isso e um piscar de lampada de 250 ms ou um blecaute de
// 3 horas. As duas coisas comecam igual. A v3.2 tentava decidir ali
// mesmo e so conseguia silenciar tudo — o teste de campo mostrou 20 de
// 20 transicoes silenciadas, ou seja, o alerta vermelho de QUEDA parou
// de existir.
//
// A saida e ADIAR a decisao por QUEDA_GRACIA_MS:
//
//   rua cai -> guarda o estado, nao manda nada
//        |
//        +-- rua voltou antes da graca --> piscar de lampada.
//        |                                 Vai no relatorio empacotado,
//        |                                 junto com os vizinhos da
//        |                                 mesma rajada. Nenhuma
//        |                                 mensagem avulsa.
//        |
//        +-- continuou cainda apos a graca --> e blecaute de verdade.
//                                          Publica QUEDA agora, com o
//                                          horario real da queda (ja
//                                          carimbado), e depois o
//                                          SERVICO ATIVO na volta.
//
// O custo e um atraso de ~8 s no alerta de blecaute real — a v3.1
// levava ~2 s. O ganho e que o piscar de lampada finalmente passa a
// existir, e numa mensagem so em vez de tres.
#define QUEDA_GRACIA_MS 8000    // espera antes de tratar como blecaute real

// Quantos pulsos estao guardados para o relatorio. Declarado aqui
// porque ha funcoes antes do bloco de pulsos que precisam deste.
static int pulsoQtd = 0;

// quedaAdiada  = a rua caiu e a graca ainda nao venceu
// alertaAvulso = ja publicamos QUEDA/SERVICO ATIVO para este episodio,
//                entao o relatorio nao deve repetir a mesma historia
static bool quedaAdiada = false;
static bool alertaAvulso = false;

// Ha evidencia de RAJADA (pulsos curtos ja registrados). Nesse caso a
// transicao de estado nao vira mensagem avulsa: vai no relatorio.
//
// Note que isto olha apenas o BUFFER. Olhar "pulso em curso" seria
// errado — todo blecaute real tambem tem um pulso em curso, e ai o
// alerta de verdade seria silenciado junto.
static bool silenciarTransicao() {
  return pulsoRelatorioPendente || pulsoQtd > 0;
}

// --- fallback para queda longa sem dur_ms (ver coreLoop) ---
// Um blecaute mais longo que o proprio heartbeat do Arduino deixa
// o sistema sem "dur_ms" para registrar. Guardamos aqui o epoch em
// que o estado virou "ausente", para o relatorio conseguir duracao.
static time_t pulsoSemEnergiaDesde = 0;
static bool pulsoQuedaContada = false;

// Uma entrada por pulso: hora (ou aproximada), duracao e profundidade
struct Pulso {
  time_t hora;
  unsigned long durMs;
  int ampMin;
};
static Pulso pulsos[PULSO_MAX];

static void pulsoRegistrar(unsigned long durMs, int ampMin) {
  time_t agora = relogioAgora();
  pulsoTotal++;

  // Este episodio ja foi contado como avulso (QUEDA + SERVICO ATIVO).
  // Guardar o pulso aqui geraria um relatorio contando a MESMA historia
  // de novo, 5 s depois.
  //
  // A linha "dur_ms" chega ANTES da de "presente", entao neste ponto
  // alertaAvulso ainda esta levantado — e e exatamente o que queremos.
  if (alertaAvulso) {
    Serial.println("Pulso ignorado (" + String(durMs) +
                   " ms) — episodio ja publicado avulsamente");
    return;
  }

  if (pulsoQtd < PULSO_MAX) {
    pulsos[pulsoQtd].hora = agora;
    pulsos[pulsoQtd].durMs = durMs;
    pulsos[pulsoQtd].ampMin = ampMin;
    pulsoQtd++;
  } else {
    // Fila cheia: descarta a mais antiga. O pulso MAIS RECENTE e o
    // que importa — ele diz o que esta acontecendo agora.
    for (int i = 1; i < pulsoQtd; i++) pulsos[i - 1] = pulsos[i];
    pulsos[pulsoQtd - 1].hora = agora;
    pulsos[pulsoQtd - 1].durMs = durMs;
    pulsos[pulsoQtd - 1].ampMin = ampMin;
  }

  // Janela de agrupamento: recomeca a cada pulso. Assim 3 pulsos em
  // 5 s viram UM relatorio, e um pulso solitario 20 s depois vira
  // outro.
  pulsoJanelaFim = millis() + JANELA_PULSO_MS;
  pulsoRelatorioPendente = true;

  Serial.print("Pulso registrado (");
  Serial.print(durMs);
  Serial.print(" ms, amp ");
  Serial.print(ampMin);
  Serial.print(") — ");
  Serial.print(pulsoQtd);
  Serial.println(" no buffer");
}

// Trata uma linha {"pulso":...} vinda do Arduino.
void processarPulso(const String& linha) {
  int dur = extrairInteiro(linha, "\"dur_ms\":");
  int amp = extrairInteiro(linha, "\"amp\":");
  if (amp < 0) amp = extrairInteiro(linha, "\"amp_min\":");

  bool abrindo = linha.indexOf("\"abrindo\":1") >= 0;

  sensorPulsou();

  if (abrindo) {
    // Pulso em curso: nao registra ainda — precisamos da duracao.
    if (pulsoInicio == 0) pulsoInicio = millis();
    if (amp >= 0) pulsoAmpMin = (pulsoAmpMin < 0 ? amp : (amp < pulsoAmpMin ? amp : pulsoAmpMin));
    return;
  }

  if (dur < 0) return;          // linha malformada

  // O Arduino mede a duracao inteira. Se tambem guardamos o inicio
  // aqui, fica o maior dos dois — o do Arduino e o mais preciso.
  unsigned long durMs = (unsigned long)dur;
  if (pulsoInicio != 0 && millis() - pulsoInicio > durMs)
    durMs = millis() - pulsoInicio;

  int prof = amp >= 0 ? amp : pulsoAmpMin;
  pulsoRegistrar(durMs, prof);
  pulsoInicio = 0;
  pulsoAmpMin = -1;
}

// Monta e envia o relatorio empacotado dos pulsos guardados.
static void enviarRelatorioPulsos() {
  // O relatorio serve para pulsos curtos E para a queda longa do
  // fallback. Sair se nao ha nada a contar.
  bool temFallback = pulsoQuedaContada && emQueda && pulsoSemEnergiaDesde > 0;
  if (pulsoQtd <= 0 && !temFallback) return;

  int flicker = 0, quedas = 0;
  unsigned long somaMs = 0;
  for (int i = 0; i < pulsoQtd; i++) {
    somaMs += pulsos[i].durMs;
    if (pulsos[i].durMs < LIMITE_RELATORIO_MS) flicker++;
    else quedas++;
  }

  // Queda longa, sem dur_ms do Arduino. Nao passa pelo loop acima:
  // ela nunca virou "pulso" porque o heartbeat parou antes de o
  // Arduino mandar o fim.
  time_t agoraRel = relogioAgora();
  unsigned long fallbackMs = 0;
  if (pulsoQuedaContada && emQueda && pulsoSemEnergiaDesde > 0 && agoraRel > pulsoSemEnergiaDesde) {
    fallbackMs = (agoraRel - pulsoSemEnergiaDesde) * 1000UL;
    quedas++;
  }
  somaMs += fallbackMs;

  // Titulo: quantos pulsos e quanto tempo no total
  int totalEventos = pulsoQtd + (pulsoQuedaContada && emQueda ? 1 : 0);
  String titulo = "RELATÓRIO DE PULSOS — " + String(totalEventos) +
                  (totalEventos == 1 ? " evento" : " eventos");
  String msg = "O sensor viu " + String(totalEventos) +
               (totalEventos == 1 ? " pulso" : " pulsos") +
               " na rede em um intervalo curto.\n\n";

  if (quedas > 0 && flicker > 0)
    msg += "Desses, " + String(quedas) + " acima de 1 s (queda de verdade)\n" +
           "e " + String(flicker) + " abaixo (piscar de lâmpada).\n\n";
  else if (quedas > 0)
    msg += "Todos acima de 1 s — queda de verdade.\n\n";
  else
    msg += "Todos abaixo de 1 s — o padrão de quem vê a lâmpada piscar.\n\n";

  msg += "Tempo total sem energia: " + relogioFormatarDuracao(somaMs / 1000) + "\n";
  msg += "Quedas desde o boot: " + String(pulsoTotal) + "\n\n";

  msg += "Detalhe:\n";
  for (int i = 0; i < pulsoQtd; i++) {
    msg += "  " + (pulsos[i].hora > 0 ? relogioFormatar(pulsos[i].hora) : String("?"));
    msg += "  " + String(pulsos[i].durMs) + " ms\n";
  }
  if (fallbackMs > 0) {
    msg += "  " + (pulsoSemEnergiaDesde > 0 ? relogioFormatar(pulsoSemEnergiaDesde) : String("?"));
    msg += "  em andamento (" + relogioFormatarDuracao(fallbackMs / 1000) + ")\n";
  }
  msg += "\n";
  // notaHora(pulsos[0].hora) estouraria no caso do fallback, onde
  // pulsos[] esta vazio; usa a hora do episodio, que vale nos dois.
  msg += notaHora(pulsoQtd > 0 ? pulsos[0].hora : pulsoSemEnergiaDesde);
  msg += identificacao();
  msg += agoraStr();

  // Cor: laranja se tem queda de verdade, amarelo se so flicker
  Serial.println("Relatorio de pulsos: " + String(totalEventos) + " eventos (" +
                 String(quedas) + " quedas, " + String(flicker) + " flicker)");
  publicar(titulo, msg, quedas > 0 ? 0xFF8C00 : 0xFFD400);

  pulsoQtd = 0;
  pulsoRelatorioPendente = false;
  pulsoJanelaFim = 0;
  // A queda longa ja foi reportada. Sem isto, o episodio entraria de
  // novo em todo relatorio seguinte enquanto durasse.
  pulsoQuedaContada = false;
}

// Relatorio horario de saude do fornecimento (prioridade 3 do
// plano). Timer contado do ULTIMO ENVIO, nao do relogio de parede:
// sem rua nao ha roteador, logo nao ha WiFi — um horario que vence
// no meio de um blecaute nao e perdido: a mensagem sai quando a
// rede voltar (apos a fila de alertas), e ai vale como confirmacao
// de retorno.
static unsigned long ultimaHoraria = 0;
static bool horariaValido = false;

// --- Acumuladores por hora (subtask 3.1 do plano) ---
// Contam desde o ultimo relatorio horario e zeram junto com ele.
//
// DECISAO: acumular no FIM da queda (nao tick a tick). Funciona porque
// o relatorio horario SO e publicado com emQueda==false — toda queda
// da janela ja terminou quando o relatorio sai. Um tick a tick daria o
// mesmo numero e custaria uma tarefa extra rodando sempre.
static int quedasHora = 0;                  // quedas encerradas na janela
static unsigned long segSemEnergiaHora = 0; // segundos totais sem rua

// --- Minimo / maximo / media da TENSAO na janela (VACmin/max/med) ---
// Amostrado a cada leitura do Arduino (~2 leituras/s) e zerado junto
// com o relatorio horario.
//
// DECISAO: so amostra quando ha sinal (amp >= 10, ou seja vrms > 0).
// Sem rua o ZMPT entrega ruido do ADC; meter "0 V" na amostra
// desmoronaria a media de uma hora em que a rede esteve 99% do tempo
// normal — e o minimo viraria sempre 0, sem dizer nada.
static float vrmsMinHora = 0.0f;
static float vrmsMaxHora = 0.0f;
static float vrmsSomaHora = 0.0f;          // soma p/ calcular a media
static unsigned long vrmsNHora = 0;        // amostras validas na janela

void sensorPulsou() {
  ultimaLeituraSensor = millis();
  if (sensorSilencioso) {
    sensorSilencioso = false;
    Serial.println("Sensor: voltou a falar");
  }
}

// --- Frequencia da rede medida pelo Arduino ---
// O Arduino calcula a frequencia e manda na linha de diagnostico
// "#ZMPT amp=... f=60.1Hz". Essa linha comeca com "#", cai fora do
// contrato JSON e o gateway a descartava — entao a frequencia era
// MEDIDA e PERDIDA no caminho. Guardamos aqui.
//
// Faixa 40..70 Hz: fora disso nao ha como saber se e ruido ou um
// cruzamento espurio. -1 = nenhuma medida valida desde o boot.
//
// ⚠ RESOLUCAO ~0,9 Hz — NAO mandar para texto humano:
// a formula do Arduino (contarCruzamentos/main_zmpt_teste.cpp) conta
// cruzamentos INTEIROS numa janela fixa de ~1500 amostras, o que
// quantiza o resultado em passos de ~0,9 Hz. Em 45 amostras de campo
// (18/10) so apareceram 4 valores: 58.0, 58.9, 59.8, 60.7. A rede real
// varia ~+-0,1 Hz. Serve para depuracao via API, NAO para o relatorio.
static float freqHz = -1.0f;

void sensorFrequencia(float hz) {
  if (hz < 40.0f || hz > 70.0f) return;   // ruido, descarta
  freqHz = hz;
}

static WiFiClientSecure clienteSeguro;
static HTTPClient http;
static WebServer servidor(80);
static WiFiClient clienteMqtt;
static PubSubClient mqtt(clienteMqtt);
static unsigned long ultimaTentativaMqtt = 0;
static unsigned long ultimoHeartbeatMqtt = 0;

// ==================================================================
//  UTILITARIOS
// ==================================================================
// Extrai o valor de uma chave de texto: "energia":"presente"
// Retorna "" se nao encontrar (ex.: linha de boot do Arduino).
String extrairTexto(const String& json, const String& chave) {
  int i = json.indexOf(chave);
  if (i < 0) return "";
  i += chave.length();
  int f = json.indexOf('"', i);
  if (f < 0) return "";
  return json.substring(i, f);
}

// Extrai um inteiro: "amp":94  -> 94   (ou -1 se nao encontrar)
int extrairInteiro(const String& json, const String& chave) {
  int i = json.indexOf(chave);
  if (i < 0) return -1;
  i += chave.length();
  int f = i;
  while (f < (int)json.length() && (isDigit(json[f]) || json[f] == '-')) f++;
  if (f == i) return -1;
  return json.substring(i, f).toInt();
}

// Identificacao do imovel (aparece em todas as mensagens)
static String identificacao() {
  return String(LOCAL_TXT) + "\n" + CEP_TXT + "\n" + UC_TXT + "\n";
}

// Relatorio horario de saude (prioridade 3): prova de vida do
// monitor + confirmacao de que o fornecimento permanece ativo.
// So sai com rua ligada, WiFi em pie e fila vazia — nunca compete
// com alerta de queda.
//
// Payload minimo do plano (prioridade 3d). Duas ausencias de proposito:
//
//  - "fila": o relatorio so e publicado quando filaQtd == 0 (gate em
//    coreLoop). Reportar "Fila: 0" seria uma tautologia.
//  - "tipo":"relatorio_hora": o Discord recebe texto, nao JSON. O JSON
//    com esse tipo pertence ao consumidor MQTT; aqui vai a leitura
//    humana do mesmo conteudo.
static void enviarRelatorioHorario() {
  // O titulo do embed e "ULTIMO PERIODO" — e o que o relatorio e.
  // A linha "Situacao: SERVICO PERMANECE ATIVO" foi removida: repetia
  // o titulo palavra por palavra.
  String msg = identificacao();
  msg += notaHora(relogioAgora()) + agoraStr() + "\n";

  // --- Telemetria da janela ---
  msg += "\n";
  if (ampAtual >= 10) {
    // amp < 10 e ruido do ADC sem rua (ver vrmsAtual): reportar
    // "2.1 V" inventado seria pior que dizer que nao ha sinal.
    msg += "Rede: " + String(vrmsAtual(), 1) + " V";
    msg += " (nominal " + String(V_NOM) + " V)\n";
  } else {
    msg += "Rede: sem sinal (rua desligada)\n";
  }
  // VACmin / VACmax / VACmed da janela de 1 h (ver vrmsMinHora..).
  // Sem amostra valida nao ha o que mostrar — nao imprime a linha.
  if (vrmsNHora > 0) {
    msg += "Nesta hora — mín " + String(vrmsMinHora, 1) +
           " V · média " + String(vrmsSomaHora / (float)vrmsNHora, 1) +
           " V · máx " + String(vrmsMaxHora, 1) + " V\n";
  }
  // FREQUENCIA NAO VAI AQUI (decisao 18/10).
  //
  // A medicao do Arduino resolve ~0,9 Hz: em 45 amostras so apareceram
  // 4 valores distintos (58.0 / 58.9 / 59.8 / 60.7). A rede real de
  // 60 Hz varia ~+-0,1 Hz e a PRODIST tolera +-0,5 Hz — entao 1 em cada
  // 5 amostras mostraria 58.9 Hz, fora de faixa, sugerindo problema na
  // RUA quando o problema e da MEDICAO. Texto pra humano nao pode
  // assustar a toa.
  //
  // freq_hz continua no /estado e no MQTT: la e campo de depuracao e o
  // consumidor sabe interpretar (e -1 = nenhuma medida valida).
  msg += "Quedas nesta hora: " + String(quedasHora);
  msg += "  ·  Sem energia: " + String(segSemEnergiaHora / 60) + " min\n";
  msg += "Uptime: " + relogioFormatarDuracao(millis() / 1000) + "\n";

  publicar("ULTIMO PERIODO", msg, 0x00FF00);   // verde

  // Fecha a janela: o proximo relatorio conta do zero.
  quedasHora = 0;
  segSemEnergiaHora = 0;
  vrmsMinHora = vrmsMaxHora = vrmsSomaHora = 0.0f;
  vrmsNHora = 0;
}

static String escapar(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  return s;
}

// Hora atual no formato das mensagens. Se nao temos NTP, o epoch
// e uma estimativa — nesse caso devolve a hora e o chamador deve
// marcar a mensagem como aproximada (ver notaHora).
static String agoraStr() {
  time_t agora = relogioAgora();
  if (agora <= 0) return String("Hora: ainda nao sincronizada");
  return String("Hora: ") + relogioFormatar(agora);
}

// Linha de aviso quando a hora nao veio do NTP.
static String notaHora(time_t t) {
  if (relogioValido(t)) return "";
  return "⚠️ hora aproximada (sem NTP no momento)\n";
}

// Duracao da queda em andamento, preferindo o relógio (o RTC
// interno) sobre o millis(): assim vale mesmo se a placa
// reiniciar no meio da queda (o checkpoint restaura o epoch).
static unsigned long duracaoQuedaMs() {
  if (tsQuedaEpoch > 0) {
    time_t agora = relogioAgora();
    if (agora > tsQuedaEpoch) return (agora - tsQuedaEpoch) * 1000UL;
  }
  return millis() - tsQueda;
}

// Tensao RMS da rede em volts, a partir da amplitude pico-a-pico.
// K_VRMS absorve AVcc, razao do ZMPT e a conversao p-p -> RMS (plan
// 2.4). Calibracao 18/10/2026: 5 pontos, K=0.2987 +-0.60%, span 4,7%
// — serve p/ monitor (volts, swell/under-voltage), NAO p/ apuracao
// DRP/DRC (exige 9 pontos PRODIST + variac). Ver docs/calibracao-zmpt.md.
//
// Sem sinal (amp ~ruido na queda) o numero nao significa tensao:
// devolve 0 em vez de um "volts" inventado em cima do ruido.
static float vrmsAtual() {
  if (ampAtual < 10) return 0.0F;   // sem rua: so ruido no ADC
  return K_VRMS * ampAtual;
}

// ==================================================================
//  WIFI
// ==================================================================
static void conectarWiFi(int esperaMs = 4000) {
  Serial.print("Conectando WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < esperaMs / 500) {
    delay(500);
    Serial.print(".");
    t++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    clienteSeguro.setInsecure();   // nao valida certificado (economiza RAM)
    Serial.println(" OK -> " + WiFi.localIP().toString());
  } else {
    wifiOk = false;
    Serial.println(" FALHOU (verifique SSID/senha — 2.4 GHz)");
  }
}

// ==================================================================
//  ENVIO DIRETO (baixo nivel)
// ==================================================================
static bool enviarPushbullet(const String& titulo, const String& corpo) {
  if (!wifiOk) return false;

  http.begin(clienteSeguro, "https://api.pushbullet.com/v2/pushes");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Access-Token", PB_TOKEN);

  String payload = "{\"type\":\"note\",\"title\":\"" + escapar(titulo) +
                   "\",\"body\":\"" + escapar(corpo) + "\"}";

  int codigo = http.POST(payload);
  Serial.println("Pushbullet: " + String(codigo));
  http.end();

  return codigo >= 200 && codigo < 300;
}

static bool enviarDiscord(const String& titulo, const String& descricao, int cor) {
  if (!wifiOk) return false;

  http.begin(clienteSeguro, DISCORD_URL);
  http.addHeader("Content-Type", "application/json");

  // SEM footer no embed: "Monitor de Quedas - Antonina-PR" agora e a
  // ultima linha do CORPO (ver assinar()), com o link da documentacao
  // logo abaixo. Com o footer, a assinatura saia DUAS vezes.
  String payload = "{\"embeds\":[{\"title\":\"" + escapar(titulo) +
                   "\",\"description\":\"" + escapar(descricao) +
                   "\",\"color\":" + String(cor) + "}]}";

  int codigo = http.POST(payload);
  Serial.println("Discord: " + String(codigo));   // 204 = ok
  http.end();

  return codigo >= 200 && codigo < 300;
}

// ==================================================================
//  FILA DE MENSAGENS
// ==================================================================
// MOTIVO: o roteador cai junto com a rua. No instante exato da
// QUEDA nao ha internet — entao a mensagem e montada com o hora
// do RTC interno, guardada aqui, e sai quando o WiFi voltar.
//
// A fila vive na RAM de proposito: se o power bank esgotar e a
// placa desligar, o CHECKPOINT na flash e quem permite reconstruir
// a mensagem no boot (ver "checkpoint" abaixo). Persistir a fila
// tambem so duplicaria esse trabalho e gastaria flash a toa.
// ==================================================================
struct Aviso {
  String titulo;
  String corpo;
  int cor;
  bool pbEnviado;   // evita mandar o Pushbullet 2x ao reentregar
};

static Aviso fila[FILA_MAX];
static int filaQtd = 0;
static bool filaAvisoLotada = false;

static bool tentarEnviar(Aviso& a) {
  // Pushbullet: melhor esforco. Marca como feito APENAS quando
  // havia rede para tentar — assim, uma mensagem que entrou na
  // fila sem internet ainda sera notificada na entrega.
  if (!a.pbEnviado && wifiOk) {
    enviarPushbullet(a.titulo, a.corpo);
    a.pbEnviado = true;   // se falhou, nao insiste: o log e o Discord
  }
  return enviarDiscord(a.titulo, a.corpo, a.cor);
}

// --- Assinatura de TODA mensagem ---
// Feita AQUI (e nao em cada chamada) para ser garantida por
// construcao: qualquer mensagem nova ja sai assinada sem lembrar de
// chamar nada.
//
// E a ultima linha do CORPO, nao o footer do embed — com o footer a
// assinatura aparecia duas vezes (corpo + rodape cinza do embed).
// O link da documentacao vem logo abaixo, como pedido.
static String assinar(String corpo) {
  while (corpo.endsWith("\n")) corpo.remove(corpo.length() - 1);
  if (corpo.length()) corpo += "\n";
  corpo += "Monitor de Quedas - Antonina-PR\n";
  corpo += "Documentação do projeto em:\n";
  corpo += DOCS_URL;
  return corpo;
}

// Faz o envio; se a rede caiu, guarda na fila para depois.
static void publicar(const String& titulo, const String& corpo, int cor) {
  // MODO MUDO: simula em bancada sem notificar ninguem. A mensagem
  // completa sai no serial — da para conferir o texto que SERIA
  // publicado (Inicio/Retorno/Duracao) sem gastar o webhook.
  if (MODO_MUDO) {
    Serial.println();
    Serial.println("=== [MODO MUDO] mensagem NAO enviada ===");
    Serial.println("Titulo: " + titulo);
    Serial.println(assinar(corpo));
    Serial.println("=== fim ===");
    return;
  }

  Aviso a;
  a.titulo = titulo;
  a.corpo = assinar(corpo);   // assinatura em TODA postagem
  a.cor = cor;
  a.pbEnviado = false;

  if (tentarEnviar(a)) return;

  if (filaQtd >= FILA_MAX) {
    // Fila cheia: descarta a MAIS ANTIGA para guardar a mais
    // recente (a queda em andamento importa mais que a antiga).
    for (int i = 1; i < filaQtd; i++) fila[i - 1] = fila[i];
    filaQtd--;
    if (!filaAvisoLotada) {
      filaAvisoLotada = true;
      Serial.println("AVISO: fila cheia — descartada a mensagem mais antiga");
    }
  }

  fila[filaQtd] = a;
  filaQtd++;
  Serial.println("Sem rede: na fila (" + String(filaQtd) + "/" +
                 String(FILA_MAX) + ") — " + titulo);
}

// Entrega UMA mensagem da frente da fila. Retorna true se foi.
// Uma por chamada de coreLoop: o POST do Discord ja leva suas
// centenas de ms, e disparar tudo de uma vez seria cortado pelo
// rate limit do webhook (5 req / 5 s).
static bool entregarDaFila() {
  if (filaQtd <= 0) return false;
  if (!tentarEnviar(fila[0])) return false;

  Serial.println("Fila: enviada — " + fila[0].titulo);
  for (int i = 1; i < filaQtd; i++) fila[i - 1] = fila[i];
  filaQtd--;
  if (filaQtd == 0) filaAvisoLotada = false;
  return true;
}

// ==================================================================
//  CHECKPOINT NA FLASH
// ==================================================================
// Guarda "esta em queda, desde quando" antes de qualquer risco de
// perda de energia. Cobre o caso: power bank esgota -> tudo
// desliga -> a rua volta -> o sistema faz boot sem NTP e sem RAM.
// Com o checkpoint + o NTP do boot, da para reconstruir a queda.
//
// Gravamos tambem o ultimo epoch visto ("quando"): ele serve de
// partida para relogioAgora() quando o boot nao tem NTP.
// ==================================================================
static Preferences pref;
static bool cpExiste = false;
static bool cpEmQueda = false;
static time_t cpDesde = 0;
static time_t cpQuando = 0;

static void salvarCheckpoint() {
  pref.putBool("emQueda", emQueda);
  pref.putUInt("desde", (uint32_t)(tsQuedaEpoch > 0 ? tsQuedaEpoch : 0));

  // So regrava o epoch se temos hora de verdade. Caso contrario
  // gravaria 0 e destruiria a base de contagem que ja estava boa.
  time_t agora = relogioAgora();
  if (agora > 0) pref.putUInt("quando", (uint32_t)agora);
}

static void carregarCheckpoint() {
  cpExiste = pref.isKey("emQueda");
  if (!cpExiste) return;
  cpEmQueda = pref.getBool("emQueda", false);
  cpDesde = (time_t)pref.getUInt("desde", 0);
  cpQuando = (time_t)pref.getUInt("quando", 0);
}

// Imprime o checkpoint — chamar DEPOIS de relogioIniciar(), quando
// o fuso horario ja esta definido (senao imprime em UTC).
static void logCheckpoint() {
  if (!cpExiste) {
    Serial.println("Checkpoint: nenhum (primeira execucao)");
    return;
  }
  Serial.println("Checkpoint: " + String(cpEmQueda ? "EM QUEDA" : "servico ativo") +
                 " desde " + relogioFormatar(cpDesde) +
                 " | ultimo epoch " + relogioFormatar(cpQuando));
}

// ==================================================================
//  ALERTA DE HARDWARE
// ==================================================================
void alertaHardware(const String& titulo, const String& corpo) {
  String msg = corpo + "\n" + identificacao() + agoraStr();
  publicar(titulo, msg, 0xFF8C00);   // laranja
}

// ==================================================================
//  OTA — ATUALIZACAO DO FIRMWARE PELA REDE
//  subtasks 1.2 .. 1.6 de docs/plano-ota-qualidade.md
// ==================================================================
//
// 1.2 TRANSPORTE: WebServer + Update (POST multipart em /update).
//     O WebServer ja roda no mesmo socket do /estado, nao traz
//     dependencia nova, e a subtask 1.1 ja garante que o socket
//     sobrevive a reconexao do WiFi (servidor.begin() no return do
//     WiFi). ArduinoOTA (mDNS) conviveria com uma segunda pilha de
//     rede e ainda exigiria espota.py + senha.
//
// 1.3 Update.begin()/write()/end() com MD5 opcional no header X-MD5.
//
// 1.4 GATE: so com rua ligada, WiFi em pie e fila de alertas vazia.
//     Um firmware novo no meio de um blecaute competiria com a
//     deteccao — a janela errada para arriscar.
//
// 1.5 ROLLBACK de verdade — e e ISTO que faz o OTA ser utilizavel sem
//     cabo na mao. O esquema, em ordem:
//       - Update.end() com a imagem inteira troca o otadata;
//       - no proximo boot a imagem nova nasce em
//         ESP_OTA_IMG_PENDING_VERIFY e o bootloader ARMADA o rollback;
//       - se ela panicar, estourar o watchdog ou reiniciar ANTES de
//         ser confirmada, o bootloader vira IMG_ABORTED e volta para a
//         imagem anterior sozinho;
//       - a confirmacao e feita AQUI, quando o nucleo PROVA que
//         funciona — esp_ota_mark_app_valid_cancel_rollback().
//
//     PEGADILHA (a que derrubou a primeira versao): o proprio nucleo
//     Arduino ja confirma a imagem em initArduino(), ANTES do setup().
//     cores/esp32/esp32-hal-misc.c:
//
//         bool verifyRollbackLater() __attribute__((weak));
//         bool verifyRollbackLater() { return false; }   // confirma cedo
//         void initArduino() {
//           if(!verifyRollbackLater()){ ... if (PENDING_VERIFY) {
//               if (verifyOta())  esp_ota_mark_app_valid_cancel_rollback();
//               else              esp_ota_mark_app_invalid_rollback_and_reboot();
//           }}}
//
//     Ambos sao __attribute__((weak)), entao sobrescrevemos aqui os
//     DOIS: verifyRollbackLater()=true (ele fica quieto e nos deixa o
//     rollback armado) e verifyOta()=true (nao deixamos ele decidir —
//     ele nao tem como saber se o SENSOR funciona). Sem esta
//     sobrescrita o estado nunca passa por PENDING e o 1.5 nao existe:
//     era exatamente o que estava acontecendo, medido no /estado.
//
//     O que a bancada ja provou (subtask 1.6):
//       - imagem valida    -> 200, reboot, slot troca app0<->app1,
//                             estado NOVA -> PENDENTE -> VALIDA;
//       - magic byte torto -> 500 "Magic byte Error", SEM reboot;
//       - MD5 errado       -> 500 "MD5 Check Failed",  SEM reboot;
//       - credencial errada-> 401, nao um byte gravado;
//       - fim estragado    -> 500 "Could Not Activate The Firmware",
//                             sem trocar o boot, SEM reboot. O erro vem
//                             do proprio Update._verifyEnd(), que le o
//                             que foi gravado antes de apontar o boot
//                             para la.
//     A queda de energia no MEIO da escrita tambem nao apaga a imagem
//     boa: o otadata so muda la no fim do Update.end(), com a imagem
//     completa e validada — se faltar luz no meio, o aparelho simples
//     volta a antiga no proximo boot. O proprio Updater reforca isso:
//     recusa qualquer arquivo que nao comece com 0xE9 ja no PRIMEIRO
//     bloco escrito e SEGURA os 16 primeiros bytes ate o fim, para
//     que uma imagem pela metade jamais pareca bootavel.
//
// 1.6 BANCADA OBRIGATORIA antes de campo: imagem valida, imagem
//     corrompida, corte no meio do envio, credencial errada, gate
//     bloqueando. Nao e opcional — e a unica forma de saber que o
//     rollback funciona antes de precisar dele.
//
// SEGURANCA: o endpoint exige Basic Auth e, se o config.h nao tiver
// credencial, nem e registrado (ver OTA_ATIVO la em cima).

#if OTA_ATIVO

// ==================================================================
//  HOOKS DO NUCLEO ARDUINO — precisam vir ANTES de tudo
// ==================================================================
// cores/esp32/esp32-hal-misc.c declara os dois como __attribute__((weak))
// e chama em initArduino(), ou seja ANTES do setup() e do coreIniciar().
// Por isso sao extern "C" e por isso estao aqui: a definicao forte
// sobrescreve a fraca do nucleo.
//
//   verifyRollbackLater() -> true
//       Sem isso o nucleo confirma a imagem sozinho no boot e o
//       PENDING_VERIFY some antes do meu codigo existir — e ai nao ha
//       mais o que confirmar nem o que reverter. Medido: /estado
//       mostrava ota_estado=VALIDA a partir do uptime 7 s.
//
//   verifyOta() -> true
//       Nao deixamos o nucleo julgar. A unica prova que interessa e a
//       nossa: o sensor tem que falar e o aparelho tem que ficar de
//       pe por OTA_CONFIRMA_MS. Verificar em initArduino() seria
//       cedo demais — nesse ponto o UART2 ainda nem foi aberto.
extern "C" bool verifyRollbackLater() { return true; }
extern "C" bool verifyOta()           { return true; }

static bool otaRecebendo = false;    // upload autorizado, gravando
static bool otaPronto = false;       // fim do envio com imagem validada
static bool otaFalhou = false;
static String otaMotivo = "";
static String otaDestino = "?";      // rotulo do slot de destino
static size_t otaBytes = 0;          // bytes efetivamente recebidos

// Instante do boot, base da janela OTA_CONFIRMA_MS antes de aceitar a
// imagem nova (1.5). O estado em si vem do otadata, nao de um flag —
// ver otaLerEstado().
static unsigned long otaBootMs = 0;

// --- 1.4 gate de seguranca ---
static bool otaPode(String& motivo) {
  if (!wifiOk)     { motivo = "sem rede"; return false; }
  if (emQueda)     { motivo = "em queda — o monitor esta precisando"; return false; }
  if (filaQtd > 0) { motivo = "fila de alertas nao vazia"; return false; }
  motivo = "";
  return true;
}

// Chamado varias vezes com pedacos do corpo. E AQUI que a imagem e
// gravada; a resposta so sai no handler final (otaResponder).
static void otaReceber() {
  HTTPUpload& up = servidor.upload();

  if (up.status == UPLOAD_FILE_START) {
    otaRecebendo = otaPronto = otaFalhou = false;
    otaMotivo = "";
    otaBytes = 0;

    // Os headers chegam ANTES do corpo: da para barrar sem gravar um
    // byte sequer. (O WebServer so guarda Authorization se
    // collectHeaders() tiver sido chamado — feito em coreIniciar.)
    if (!servidor.authenticate(OTA_USUARIO, OTA_SENHA)) {
      otaFalhou = true;
      otaMotivo = "credenciais recusadas";
      Serial.println("OTA: recusado — credenciais erradas");
      return;
    }
    String gate;
    if (!otaPode(gate)) {
      otaFalhou = true;
      otaMotivo = gate;
      Serial.println("OTA: recusado — " + gate);
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaFalhou = true;
      otaMotivo = String("sem espaco: ") + Update.errorString();
      Serial.println("OTA: " + otaMotivo);
      return;
    }
    const esp_partition_t* alvo = esp_ota_get_next_update_partition(NULL);
    otaDestino = alvo ? String(alvo->label) : String("?");

    // MD5 opcional (header X-MD5). Se vier e nao bater, Update.end()
    // recusa e o otadata NAO troca — a imagem boa continua ativa.
    String md5 = servidor.header("X-MD5");
    if (md5.length() == 32) {
      Update.setMD5(md5.c_str());
      Serial.println("OTA: MD5 esperado " + md5);
    } else {
      Serial.println("OTA: sem X-MD5 — validacao so na integridade do fim");
    }
    otaRecebendo = true;
    Serial.println("OTA: recebendo no slot " + otaDestino + " (" +
                   String(ESP.getFreeSketchSpace()) + " B livres)");
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaRecebendo) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.abort();
      otaRecebendo = false;
      otaFalhou = true;
      // O motivo vem do proprio Updater: "Magic byte Error" quando o
      // arquivo nem comeca com 0xE9, "Flash Error" quando a gravacao
      // de falha etc. Melhor do que um generico "falha ao gravar".
      otaMotivo = "falha ao gravar: " + String(Update.errorString());
      Serial.println("OTA: falha no slot " + otaDestino + " — " + otaMotivo);
    }
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (!otaRecebendo) return;
    otaRecebendo = false;
    otaBytes = up.totalSize;
    // evenIfRemaining = true: o tamanho declarado e o do SLOT INTEIRO
    // (1.310.720 B) e a imagem e menor — sem isso o fim recusaria.
    if (Update.end(true)) {
      otaPronto = true;
      Serial.println("OTA: imagem validada — " + String(otaBytes) +
                     " bytes, MD5 " + Update.md5String());
    } else {
      otaFalhou = true;
      otaMotivo = Update.errorString();
      Serial.println("OTA: recusada no fim — " + otaMotivo);
    }
  }
  else if (up.status == UPLOAD_FILE_ABORTED) {
    if (otaRecebendo) Update.abort();
    otaRecebendo = false;
    otaFalhou = true;
    otaMotivo = "cancelado pelo cliente";
    Serial.println("OTA: envio cancelado");
  }
}

// Resposta do POST — roda DEPOIS de todo o corpo ter passado por
// otaReceber(). Aqui decidimos se reinicia ou nao.
static void otaResponder() {
  if (!servidor.authenticate(OTA_USUARIO, OTA_SENHA)) {
    servidor.requestAuthentication();
    return;
  }

  if (otaPronto) {
    servidor.send(200, "application/json",
                  "{\"ok\":true,\"bytes\":" + String(otaBytes) +
                  ",\"md5\":\"" + Update.md5String() +
                  "\",\"slot\":\"" + otaDestino +
                  "\",\"reiniciando\":true}");

    // Publica ANTES de reiniciar: depois do restart nao ha WiFi para
    // entregar a mensagem. publicar() envia sincrono com rede em pie.
    String m = "Firmware atualizado com sucesso\n";
    m += "Tamanho: " + String(otaBytes) + " bytes\n";
    m += "MD5: " + Update.md5String() + "\n";
    m += "Slot: " + otaDestino + "\n\n";
    m += "Reiniciando. A nova imagem só é confirmada depois de provar\n";
    m += "que funciona (30 s + sensor falando). Se ela falhar antes\n";
    m += "disso, o bootloader volta sozinho para a versão anterior.\n";
    publicar("OTA — FIRMWARE ATUALIZADO", m, 0x00FFFF);   // ciano

    delay(600);   // deixa o TCP entregar a resposta antes do corte
    Serial.println("OTA: reiniciando no slot " + otaDestino);
    ESP.restart();
    return;
  }

  if (otaFalhou) {
    servidor.send(500, "application/json",
                  "{\"ok\":false,\"erro\":\"" + escapar(otaMotivo) + "\"}");
    return;
  }
  servidor.send(400, "application/json",
                "{\"ok\":false,\"erro\":\"nenhuma imagem recebida\"}");
}

#endif  // OTA_ATIVO

// --- expostos para fora (rotaEstado + MQTT) ---
//
// Sem estes campos nao da para saber, de fora do aparelho, QUAL imagem
// esta rodando, se o rollback ainda esta armado ou QUE ESTADO o
// otadata registrou — e seria impossivel depurar uma troca de slot ou
// um rollback sem estar com o cabo na mao.
//
// otaLerEstado() faz a leitura de verdade e guarda em cache: chamar
// esp_ota_get_state_partition() a cada volta do loop seria um acesso
// a flash sem necessidade; aqui atualiza 1x por segundo (coreLoop) e
// na demanda do /estado.
static esp_ota_img_states_t otaEstadoCache = ESP_OTA_IMG_UNDEFINED;
static bool otaEstadoValido = false;

static void otaLerEstado() {
  const esp_partition_t* p = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) {
    otaEstadoValido = false;
    return;
  }
  otaEstadoValido = true;
  otaEstadoCache = st;
}

// Texto do estado cru, para o JSON. 0..4 sao os valores de
// esp_ota_img_states_t (esp_flash_partitions.h).
static String otaEstadoStr() {
  if (!otaEstadoValido) return "SEM_REGISTRO";
  switch (otaEstadoCache) {
    case ESP_OTA_IMG_NEW:            return "NOVA";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDENTE";   // rollback ARMADO
    case ESP_OTA_IMG_VALID:          return "VALIDA";     // confirmada
    case ESP_OTA_IMG_INVALID:        return "INVALIDA";
    case ESP_OTA_IMG_ABORTED:        return "ABORTADA";   // bootloader recusou
    case ESP_OTA_IMG_UNDEFINED:      return "INDEFINIDA";
    default:                         return "OUTRO_" + String((int)otaEstadoCache);
  }
}

static String otaSlotAtual() {
  const esp_partition_t* p = esp_ota_get_running_partition();
  return p ? String(p->label) : String("?");
}

// true ENQUANTO o otadata diz que a imagem atual nao foi confirmada:
// qualquer reset nesse estado volta para a versao anterior (1.5).
static bool otaPendente() {
  return otaEstadoValido && otaEstadoCache == ESP_OTA_IMG_PENDING_VERIFY;
}

// ==================================================================
//  MQTT (base do futuro app Android)
// ==================================================================
static void mqttPublicarEstado() {
#if MQTT_ATIVO
  if (MODO_MUDO) {
    Serial.println("[MODO MUDO] MQTT estado NAO publicado");
    return;
  }
  if (!mqtt.connected()) return;

  String payload = "{\"energia\":\"" + estadoAtual + "\"";
  payload += ",\"amp\":" + String(ampAtual);
  payload += ",\"vrms\":" + String(vrmsAtual(), 1);
  payload += ",\"v_nom\":" + String(V_NOM);
  // -1 = nenhuma medida valida (ver sensorFrequencia). Reportar -1 e
  // melhor que omitir: o consumidor distingue "medido ruim" de
  // "nao medido".
  payload += ",\"freq_hz\":" + String(freqHz, 1);
  payload += ",\"em_queda\":" + String(emQueda ? "true" : "false");
  payload += ",\"sinal_saturado\":" + String(sinalSaturado ? "true" : "false");
  payload += ",\"ts\":" + String((unsigned long)relogioAgora());
  payload += ",\"duracao_s\":" + String((unsigned long)(emQueda ? duracaoQuedaMs() / 1000 : 0));
  payload += ",\"ultima_queda_s\":" + String(durUltimaQueda / 1000);
  payload += ",\"fila\":" + String(filaQtd);
  payload += ",\"slot\":\"" + otaSlotAtual() + "\"";
  payload += ",\"ota_estado\":\"" + otaEstadoStr() + "\"";
  payload += ",\"ota_pendente\":" + String(otaPendente() ? "true" : "false");
  payload += "}";

  bool ok = mqtt.publish(MQTT_TOPICO_ESTADO, payload.c_str(), true);
  Serial.println("MQTT estado: " + String(ok ? "ok" : "FALHOU") + " -> " + payload);
#endif
}

static void mqttLoop() {
#if MQTT_ATIVO
  if (!wifiOk) return;
  // No mudo nem conecta: senao publicaria "online" no topico de
  // status no meio do teste.
  if (MODO_MUDO) return;

  if (!mqtt.connected()) {
    unsigned long agora = millis();
    if (agora - ultimaTentativaMqtt < 5000) return;   // no maximo 1x/5s
    ultimaTentativaMqtt = agora;

    String id = "monitor-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);
    bool ok;
    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS,
                        MQTT_TOPICO_STATUS, 0, true, "offline");
    } else {
      ok = mqtt.connect(id.c_str(), MQTT_TOPICO_STATUS, 0, true, "offline");
    }

    if (ok) {
      Serial.println("MQTT conectado: " + String(MQTT_BROKER));
      mqtt.publish(MQTT_TOPICO_STATUS, "online", true);
      // se ja temos estado, republica (cobri reinicio do broker)
      if (estadoInicialEnviado) mqttPublicarEstado();
    } else {
      Serial.println("MQTT falhou (rc=" + String(mqtt.state()) + ")");
    }
    return;
  }

  mqtt.loop();

  // heartbeat: republica o estado a cada 10 min
  if (estadoInicialEnviado && millis() - ultimoHeartbeatMqtt > 10UL * 60 * 1000) {
    ultimoHeartbeatMqtt = millis();
    mqttPublicarEstado();
  }
#endif
}

// ==================================================================
//  SERVIDOR HTTP (/, /estado)
// ==================================================================
static void rotaRaiz() {
  String cor, titulo;
  if (!estadoInicialEnviado) {
    cor = "#f9a825"; titulo = "AGUARDANDO ESTADO";
  } else if (emQueda) {
    cor = "#e53935"; titulo = "QUEDA";
  } else {
    cor = "#43a047"; titulo = "SERVIÇO ATIVO";
  }

  String sub;
  if (!estadoInicialEnviado) {
    sub = "Aguardando a 1ª leitura do Arduino";
  } else if (emQueda) {
    sub = "Desde " + horaMudanca + " — " + relogioFormatarDuracao(duracaoQuedaMs() / 1000);
  } else {
    sub = "Desde " + horaMudanca;
  }

  String rodape = "amp: " + String(ampAtual) +
                  " | fila: " + String(filaQtd) +
                  " | IP: " + WiFi.localIP().toString() +
                  " | uptime: " + relogioFormatarDuracao(millis() / 1000);

  String html =
    "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>Monitor de Energia</title>"
    "<style>body{background:#111;color:#eee;font-family:system-ui,sans-serif;"
    "display:flex;align-items:center;justify-content:center;min-height:90vh;margin:0}"
    ".c{text-align:center}h1{font-size:2.4rem;margin:.2em 0}"
    ".s{color:#bbb}.r{color:#666;font-size:.85rem;margin-top:1.5em}</style></head>"
    "<body><div class='c'><h1 style='color:" + cor + "'>" + titulo + "</h1>"
    "<div class='s'>" + sub + "</div>"
    "<div class='r'>" + rodape + "</div></div></body></html>";

  servidor.send(200, "text/html", html);
}

static void rotaEstado() {
  String json = "{\"energia\":\"" + estadoAtual + "\"";
  json += ",\"amp\":" + String(ampAtual);
  json += ",\"vrms\":" + String(vrmsAtual(), 1);
  json += ",\"v_nom\":" + String(V_NOM);
  json += ",\"freq_hz\":" + String(freqHz, 1);   // -1 = sem medida valida
  json += ",\"em_queda\":" + String(emQueda ? "true" : "false");
  // (o "amp" estava duplicado aqui — bug cosmético corrigido; parsers
  // padrao ficam com o primeiro, entao remover o segundo nao muda nada)
  json += ",\"sinal_saturado\":" + String(sinalSaturado ? "true" : "false");
  json += ",\"desde\":\"" + horaMudanca + "\"";
  json += ",\"duracao_s\":" + String((unsigned long)(emQueda ? duracaoQuedaMs() / 1000 : 0));
  json += ",\"ultima_queda_s\":" + String(durUltimaQueda / 1000);
  json += ",\"fila\":" + String(filaQtd);
  json += ",\"hora_conf\":" + String(relogioConfia() ? "true" : "false");
  json += ",\"uptime_s\":" + String(millis() / 1000);
  json += ",\"wifi_rssi\":" + String(wifiOk ? WiFi.RSSI() : 0);
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"slot\":\"" + otaSlotAtual() + "\"";
  json += ",\"ota_estado\":\"" + otaEstadoStr() + "\"";
  json += ",\"ota_pendente\":" + String(otaPendente() ? "true" : "false");
  json += ",\"firmware\":\"" + nomeFw + "\"}";

  servidor.send(200, "application/json", json);
}

// ==================================================================
//  LOGICA DE QUEDA / SERVICO ATIVO
// ==================================================================
// Contrato mantido da v2: energia "presente" = rua alimentada,
// "ausente" = rua sem servico. Na v3 esses dois valem, porque o
// power bank so assume a alimentacao quando a rua cai.
// ==================================================================
void processarEstado(const String& energia, int amp) {
  if (energia != "presente" && energia != "ausente") return;

  sensorPulsou();                              // batimento do watchdog
  ampAtual = amp;                              // amplitude do ZMPT

  // Amostra de tensao para o VACmin/max/med do relatorio horario.
  // vrmsAtual() devolve 0 quando amp < 10 (sem rua = ruido), entao o
  // filtro "v > 0" descarta a rua caida sem precisar checar de novo.
  float vAmostra = vrmsAtual();
  if (vAmostra > 0.0f) {
    if (vrmsNHora == 0 || vAmostra < vrmsMinHora) vrmsMinHora = vAmostra;
    if (vrmsNHora == 0 || vAmostra > vrmsMaxHora) vrmsMaxHora = vAmostra;
    vrmsSomaHora += vAmostra;
    vrmsNHora++;
  }

  time_t agora = relogioAgora();

  // --- 1a leitura apos o boot ---
  if (!estadoInicialEnviado) {
    estadoInicialEnviado = true;
    estadoAtual = energia;
    emQueda = (energia == "ausente");
    tsQueda = millis();
    tsMudanca = millis();
    // Boot no meio de um blecaute: conta como queda da janela atual.
    // Sem isto, o "sem energia" do proximo relatorio nao bateria com
    // a experiencia de quem viu as luzes cairem.
    if (emQueda) quedasHora++;
    horaMudanca = emQueda && cpExiste && cpDesde > 0
                    ? relogioFormatar(cpDesde)
                    : (agora > 0 ? relogioFormatar(agora) : "-");

    // Caso especial: o checkpoint diz que ESTAVAMOS em queda e a
    // 1a leitura ja mostra servico de volta. Ou seja, a placa
    // desligou no meio da queda (power bank esgotou) e so voltou
    // quando a rua voltou. Reconstruimos o episodio.
    if (cpExiste && cpEmQueda && !emQueda && cpDesde > 0) {
      tsQuedaEpoch = cpDesde;
      time_t fim = agora > cpDesde ? agora : 0;
      durUltimaQueda = fim ? (fim - cpDesde) * 1000UL : 0;

      String titulo = "QUEDA (recuperada)";
      String msg = "O sistema se desligou no meio da queda (bateria esgotada)\n";
      msg += "e só voltou quando a energia retornou.\n\n";
      msg += "Início da queda: " + relogioFormatar(cpDesde) + "\n";
      msg += "Sistema reiniciou: " + (fim ? relogioFormatar(fim) : String("?")) + "\n";
      if (durUltimaQueda > 0)
        msg += "Duração (mínima): " + relogioFormatarDuracao(durUltimaQueda / 1000) + "\n";
      msg += "Observação: o sistema ficou desligado durante parte do episódio,\n";
      msg += "então a duração acima é o intervalo entre o início da queda e o\n";
      msg += "boot — não necessariamente o instante exato do retorno.\n\n";
      msg += notaHora(cpDesde);
      msg += identificacao();
      msg += agoraStr();

      Serial.println("ESTADO INICIAL: queda recuperada (desde " +
                     relogioFormatar(cpDesde) + ")");
      publicar(titulo, msg, 0xFF0000);
      salvarCheckpoint();
      mqttPublicarEstado();
      return;
    }

    // Caso normal: publica o estado atual (cobre "tudo desligou,
    // ao voltar o boot informa se esta em queda ou nao").
    tsQuedaEpoch = emQueda && cpExiste && cpEmQueda && cpDesde > 0 ? cpDesde : agora;

    String titulo = emQueda ? String("QUEDA") : String("SERVIÇO ATIVO");
    String msg = "Estado ao iniciar o sistema\n";
    msg += "Situação: " + titulo + "\n";
    if (ampAtual >= 10)
      msg += "Rede: " + String(vrmsAtual(), 1) + " V (nominal " +
             String(V_NOM) + " V)\n";
    if (emQueda && cpExiste && cpEmQueda && cpDesde > 0) {
      msg += "Queda em andamento desde: " + relogioFormatar(cpDesde) + "\n";
      msg += notaHora(cpDesde);
    }
    msg += identificacao();
    msg += agoraStr();

    Serial.println("ESTADO INICIAL: " + titulo);
    publicar(titulo, msg, emQueda ? 0xFF0000 : 0x00FF00);
    salvarCheckpoint();
    mqttPublicarEstado();
    return;
  }

  // --- Sem servico -> QUEDA ---
  if (energia == "ausente" && !emQueda) {
    emQueda = true;
    estadoAtual = energia;
    tsQueda = millis();
    tsQuedaEpoch = agora;
    tsMudanca = millis();
    horaMudanca = agora > 0 ? relogioFormatar(agora) : "-";
    quedasHora++;   // acumulador do relatorio horario (subtask 3.1)

    // A hora JA e carimbada aqui, antes da graca. E por isso que o
    // alerta publicado 8 s depois continua com o horario certo da
    // queda, nao o da confirmacao.
    if (silenciarTransicao()) {
      Serial.println("EVENTO: QUEDA (amp=" + String(ampAtual) +
                     ") [rajada — vai no relatorio]");
    } else {
      quedaAdiada = true;
      Serial.println("EVENTO: QUEDA (amp=" + String(ampAtual) +
                     ") [adiado " + String(QUEDA_GRACIA_MS / 1000) +
                     " s — se virar blecaute, publica]");
    }

    salvarCheckpoint();
    mqttPublicarEstado();   // MQTT SEMPRE: o app le o estado atual
    return;
  }

  // --- Energia de volta -> SERVICO ATIVO ---
  if (energia == "presente" && emQueda) {
    time_t inicio = tsQuedaEpoch > 0 ? tsQuedaEpoch : agora;
    durUltimaQueda = (agora > inicio) ? (agora - inicio) * 1000UL
                                      : duracaoQuedaMs();
    emQueda = false;
    estadoAtual = energia;
    tsMudanca = millis();
    horaMudanca = agora > 0 ? relogioFormatar(agora) : "-";
    // Acumulador do relatorio horario (subtask 3.1). Feito aqui, no
    // fechamento da queda — o relatorio so sai com rua ligada, entao
    // toda queda da janela ja fechou nesse ponto.
    segSemEnergiaHora += durUltimaQueda / 1000UL;

    String msg = "Energia restabelecida\n";
    msg += "Início da queda: " + (inicio > 0 ? relogioFormatar(inicio) : String("?")) + "\n";
    msg += "Retorno: " + (agora > 0 ? relogioFormatar(agora) : String("?")) + "\n";
    msg += "Duração: " + relogioFormatarDuracao(durUltimaQueda / 1000) + "\n";
    // Tensao de volta: mostra os volts, nao so a amplitude crua —
    // confirma de cara se a rede retornou NORMAL ou baixa. Mesmo
    // formato do relatorio horário. amp < 10 = so ruido do ADC, entao
    // nao ha tensao para reportar (ver vrmsAtual).
    if (ampAtual >= 10)
      msg += "Rede: " + String(vrmsAtual(), 1) + " V (nominal " +
             String(V_NOM) + " V)\n";
    msg += notaHora(inicio);
    msg += identificacao();
    msg += agoraStr();

    // Regra em espelho com a da QUEDA:
    //  - a graca venceu e publicamos QUEDA -> fecha o episodio com
    //    SERVICO ATIVO, e o relatorio fica de fora (senao repetiria
    //    a mesma historia)
    //  - a rua voltou antes da graca -> piscar de lampada; nao ha
    //    par para fechar, e o relatorio carrega o episodio
    if (quedaAdiada) {
      quedaAdiada = false;                    // voltou antes da graca
      Serial.println("EVENTO: SERVICO ATIVO (queda de " +
                     relogioFormatarDuracao(durUltimaQueda / 1000) +
                     ") [cancelado — virou relatorio]");
    } else if (alertaAvulso) {
      alertaAvulso = false;
      Serial.println("EVENTO: SERVICO ATIVO (queda de " +
                     relogioFormatarDuracao(durUltimaQueda / 1000) + ")");
      publicar("SERVIÇO ATIVO", msg, 0x00FF00);   // verde
    } else {
      Serial.println("EVENTO: SERVICO ATIVO (queda de " +
                     relogioFormatarDuracao(durUltimaQueda / 1000) +
                     ") [rajada — vai no relatorio]");
    }
    tsQuedaEpoch = 0;
    salvarCheckpoint();
    mqttPublicarEstado();
  }
}

// ==================================================================
//  CICLO DE VIDA
// ==================================================================
void coreIniciar(const char* nome) {
  nomeFw = nome;
  Serial.begin(115200);
  delay(100);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // 1) checkpoint ANTES do relogio: ele e a partida da contagem
  //    quando o boot nao consegue falar com o NTP.
  pref.begin("monitor", false);
  carregarCheckpoint();
  if (cpQuando > 0) relogioDefinirBase(cpQuando);

  // 2) rede PRIMEIRO: o NTP so responde se ja houver WiFi. Se
  //    chamasse relogioIniciar() antes, a espera toda seria gasta
  //    num sync impossivel (caso tipico: boot no meio de uma
  //    queda, com o roteador morto junto com a rua).
  conectarWiFi();

  // 3) tempo: NTP quando der, RTC interno enquanto estiver vivo
  relogioIniciar();
  logCheckpoint();

  // 4) prova de vida (se cair antes do WiFi, vai para a fila)
  String msg = "Monitor de quedas iniciado (" + nomeFw + ")\n";
  if (cpExiste)
    msg += "Último estado conhecido: " + String(cpEmQueda ? "EM QUEDA" : "serviço ativo") + "\n";
  if (!relogioConfia()) msg += "⚠️ sem NTP no boot — hora aproximada\n";
  msg += identificacao() + agoraStr();
  publicar("SISTEMA ONLINE", msg, 0x00FFFF);   // ciano

  // Primeiro relatorio HORARIA_MS depois do boot; a partir dai,
  // HORARIA_MS apos cada envio (ver coreLoop).
  ultimaHoraria = millis();
  horariaValido = true;

  // 5) servidor HTTP
  //
  // collectHeaders SEMPRE: e ele que faz o WebServer guardar o header
  // Authorization — sem, authenticate() nunca acha a credencial e
  // TODA tentativa de /update seria recusada (fail-safe, mas inutil).
  // O X-MD5 entra junto para a validacao opcional da imagem.
  const char* headersOta[] = {"X-MD5"};
  servidor.collectHeaders(headersOta, 1);
  servidor.on("/", []() { rotaRaiz(); });
  servidor.on("/estado", []() { rotaEstado(); });
  // Le o estado cru do otadata SEMPRE (mesmo com OTA desligado): o
  // /estado e o MQTT mostram ota_estado, e "SEM_REGISTRO" so seria
  // verdade se a leitura nao pudesse ser feita.
  otaLerEstado();
#if OTA_ATIVO
  servidor.on("/update", HTTP_POST,
              []() { otaResponder(); },   // resposta apos o corpo
              []() { otaReceber(); });    // pedacos da imagem
  // 1.5 — le o estado CRU do otadata no boot.
  //
  // NAO testamos um enum fixo aqui: a confianca no rollback tem de
  // vir do proprio otadata. Imprime sempre, porque esta e a unica
  // linha que diz se o 1.5 esta armado ou nao — sem ela so da para
  // descobrir isso com o cabo na mao.
  otaBootMs = millis();   // janela de confirmacao conta daqui
  Serial.println("OTA: estado da imagem = " + otaEstadoStr() +
                 " (slot " + otaSlotAtual() + ")");
#endif
  servidor.begin();

  // 6) MQTT
#if MQTT_ATIVO
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setBufferSize(512);   // payload do JSON de estado
#endif
}

void coreLoop() {
  relogioLoop();

  // --- LED de batimento (pisca a cada 2s) ---
  static unsigned long ultimoPisca = 0;
  if (millis() - ultimoPisca > 2000) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    ultimoPisca = millis();
  }

  // --- Reconecta WiFi se caiu ---
  // MOTIVO: o roteador morre junto com a rua. Varrendo canais o
  // ESP32 puxa ~250 mA contra ~120 mA com o WiFi só parado — e o
  // caso real (blecaute) e justamente quando a bateria importa.
  //
  // Por isso: escada 5/10/20/40/80/180 s, com o radio DESLIGADO
  // entre as tentativas. A primeira tentativa e curta (4 s) para
  // blecautes rapidos em que o roteador ja voltou; as seguintes
  // vao afastando. Toda tentativa e bloqueante mas curta, e so
  // acontece com a rua fora — com energia ligada nao ha varredura.
  static bool primeiraTentativa = true;
  static unsigned long ultimaRecon = 0;
  static unsigned long esperaRecon = 5000;   // escada, em ms
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiOk) {
      Serial.println("WiFi caiu (roteador junto com a rua?) — mensagens vao para a fila");
      esperaRecon = 5000;                    // reinicia a escada
      primeiraTentativa = true;
    }
    wifiOk = false;
    if (primeiraTentativa || millis() - ultimaRecon > esperaRecon) {
      primeiraTentativa = false;
      ultimaRecon = millis();
      conectarWiFi(esperaRecon == 5000 ? 4000 : 3000);
      // Escada: 5, 10, 20, 40, 80, 180, 180, ...
      esperaRecon = esperaRecon >= 180000 ? 180000 : esperaRecon * 2;
    } else {
      // Rádio desligado enquanto espera: economia real de corrente.
      if (WiFi.getMode() != WIFI_OFF) {
        Serial.println("WiFi: radio desligado ate a proxima tentativa (" +
                       String(esperaRecon / 1000) + " s)");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      }
    }
  } else {
    if (!wifiOk) {
      Serial.println("WiFi: voltou");
      // Reabre o socket de escuta do WebServer: WIFI_OFF no blecaute
      // destruiu o socket aberto no boot (subtask 1.1). Sem isto o
      // handleClient() atende em socket morto -> connection refused.
      servidor.begin();
    }
    wifiOk = true;
  }

  // --- Rede voltou: entrega UMA mensagem da fila por passo.
  //     Espaco de 1 s entre as entregas para nao levar 429 do
  //     webhook do Discord (limite de ~5 req / 5 s) e para nao
  //     bloquear a leitura da UART por segundos seguidos.
  static unsigned long ultimaEntregaFila = 0;
  if (wifiOk && filaQtd > 0 &&
      millis() - ultimaEntregaFila > 1000) {
    ultimaEntregaFila = millis();
    entregarDaFila();
  }

  // --- Relatorio horario de saude (prioridade 3) ---
  // Contado do ultimo ENVIO, nao do relógio de parede: no blecaute
  // nao ha WiFi entao nada sai — quando a rede volta, o horario ja
  // venceu e a mensagem sai sozinha, apos a fila de alertas (que
  // tem prioridade: so publica com fila vazia). Enquanto ha queda
  // em andamento, suprime: o alerta de QUEDA ja cobre o estado.
  if (horariaValido && millis() - ultimaHoraria > HORARIA_MS) {
    if (!emQueda && wifiOk && filaQtd == 0 && estadoInicialEnviado) {
      ultimaHoraria = millis();
      enviarRelatorioHorario();
    }
  }

  // --- Checkpoint periodico (mantem o epoch corrente na flash) ---
  static unsigned long ultimoCheckpoint = 0;
  if (millis() - ultimoCheckpoint > CHECKPOINT_MS) {
    ultimoCheckpoint = millis();
    if (estadoInicialEnviado) salvarCheckpoint();
  }

  // (Republicação do link da documentação no Discord — REMOVIDA a
  //  pedido: o URL e o texto de rodapé poluíam a conversa. O link
  //  segue no README e nos docs do repo.)

  // --- Watchdog: Arduino parou de falar? ---
  // O sensor manda uma linha a cada ~500 ms. Se o Arduino morrer
  // (fio solto, reset em loop, porta USB caiu), o gateway avisa
  // em vez de ficar publicando um estado que nao muda mais.
#if AVISAR_SEM_SENSOR
  if (estadoInicialEnviado && ultimaLeituraSensor > 0 && !sensorSilencioso &&
      millis() - ultimaLeituraSensor > SENSOR_SILENCIO_MS) {
    sensorSilencioso = true;
    String msg = "O sensor parou de enviar dados ha mais de " +
                 relogioFormatarDuracao(SENSOR_SILENCIO_MS / 1000) + ".\n";
    msg += "O estado exibido pode estar desatualizado.\n\n";
    msg += identificacao() + agoraStr();
    Serial.println("ALERTA: sensor em silencio");
    alertaHardware("⚠️ SENSOR EM SILÊNCIO", msg);
  }
#endif

  // --- Graca: a rua caiu e continua caindo? ---
  //
  // Este e o unico lugar que publica QUEDA. processarEstado so carimba
  // o horario e levanta quedaAdiada; a decisao de virar alerta e daqui,
  // porque so aqui da para saber se ja passou tempo suficiente para
  // descartar "piscar de lampada".
  if (quedaAdiada && emQueda && millis() - tsQueda >= QUEDA_GRACIA_MS) {
    quedaAdiada = false;
    alertaAvulso = true;

    time_t inicio = tsQuedaEpoch;
    String msg = "Sem energia na rua\n";
    msg += "Início: " + (inicio > 0 ? relogioFormatar(inicio) : String("?")) + "\n";
    msg += "Duração até agora: " + relogioFormatarDuracao((millis() - tsQueda) / 1000) + "\n";
    msg += "Detecção: o sensor não vê mais a rede elétrica\n";
    msg += notaHora(inicio);
    msg += identificacao();
    msg += agoraStr();

    Serial.println("QUEDA confirmada como blecaute real — publicando alerta");
    publicar("QUEDA", msg, 0xFF0000);   // vermelho
  }

  // Fallback para queda longa sem dur_ms.
  //
  // Se o Arduino cai junto com a rua (ou o ESP32 reinicia no meio
  // da queda), a linha "dur_ms" nunca chega. Registrariamos zero
  // pulsos e o relatorio nao sairia — mesmo tendo havido uma queda.
  // A saida aqui e o estado CONFIRMADO: "ausente" so sai do Arduino
  // depois de CONFIRMACOES leituras, e o ESP32 so move o estado
  // depois de CONFIRMACOES do proprio nucleo. Sao duas camadas de
  // debounce, entao flicker nao chega aqui.
  //
  // NAO manda o relatorio aqui de proposito: enviar agora quebraria
  // a janela de 5 s em varios relatorios (3 flips em 5 s viravam 3
  // mensagens). So armamos o marcador; quem entrega e o bloco de
  // janela fechada la embaixo.
  //
  // Tambem nao armapos se a graca ja publicou QUEDA (alertaAvulso):
  // nesse caso o Discord ja recebeu o alerta, e um relatorio contando
  // a mesma coisa seria ruido.
  if (!emQueda && pulsoQuedaContada) pulsoQuedaContada = false;   // voltou

  if (emQueda && !pulsoQuedaContada && pulsoInicio == 0 && !alertaAvulso) {
    pulsoQuedaContada = true;
    time_t agora = relogioAgora();
    if (agora > 0) pulsoSemEnergiaDesde = agora;
    Serial.println("Queda confirmada sem dur_ms do Arduino — fallback armado");
  }

  // --- Relatorio de pulsos quando a janela fechou ---
  // A janela "renova" a cada pulso novo, entao so fecha quando passa
  // 5 s sem pulso nenhum — ai os pulsos guardados saem juntos, numa
  // mensagem so. Fica ANTES do return de wifiOk: precisa sair mesmo
  // com a rede fora (entra na fila do Discord).
  //
  // O fallback (queda longa sem dur_ms) entra pelo MESMO caminho: se
  // o relatorio disparasse no instante do fallback, os pulsos curtos
  // anteriores ficariam de fora. Note que o fallback NAO exige
  // pulsoQtd: uma queda longa pura, sem nenhum pulso curto, tambem
  // tem de virar relatorio — e e o caso que mais importa.
  bool fallbackPendente = pulsoQuedaContada && emQueda &&
                          pulsoSemEnergiaDesde > 0;
  if (pulsoRelatorioPendente && pulsoQtd > 0 && millis() > pulsoJanelaFim) {
    enviarRelatorioPulsos();
  } else if (fallbackPendente && millis() > pulsoJanelaFim) {
    enviarRelatorioPulsos();
  }

  if (!wifiOk) return;

#if OTA_ATIVO
  // --- 1.5 confirma a imagem nova e desarma o rollback ---
  //
  // O estado do otadata e a FONTE DA VERDADE (otaLerEstado/otaPendente)
  // e e relido 1x por segundo: se o bootloader marcar PENDING_VERIFY
  // depois do boot, pegamos — nao ficamos com um flag preso no false.
  //
  // NAO confirmamos no boot de proposito: precisa de TEMPO DE VIDA e
  // do SENSOR FALANDO. Confirmar cedo demais assinaria um firmware que
  // panicou ou que nao le a UART — exatamente os casos em que o
  // rollback existe.
  static unsigned long otaEstadoMs = 0;
  if (millis() - otaEstadoMs >= 1000) {
    otaEstadoMs = millis();
    otaLerEstado();
  }
  static bool otaJaTentou = false;
  if (!otaJaTentou && otaPendente() && ultimaLeituraSensor > 0 &&
      millis() - otaBootMs >= OTA_CONFIRMA_MS) {
    otaJaTentou = true;   // tenta UMA vez: repetir nao muda o estado
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      otaLerEstado();
      Serial.println("OTA: imagem CONFIRMADA valida (slot " +
                     otaSlotAtual() + ", estado " + otaEstadoStr() + ")");
      String m = "Firmware confirmado — rollback cancelado\n";
      m += "Slot: " + otaSlotAtual() + "\n";
      m += "Foi o primeiro boot desta imagem; ela passou na prova\n";
      m += "(30 s de vida + sensor falando).\n";
      publicar("OTA — FIRMWARE CONFIRMADO", m, 0x00FF00);   // verde
    } else {
      Serial.println("OTA: confirmacao REJEITADA (estado " +
                     otaEstadoStr() + ") — rollback segue ARMADO");
    }
  }
#endif

  servidor.handleClient();
  mqttLoop();
}
