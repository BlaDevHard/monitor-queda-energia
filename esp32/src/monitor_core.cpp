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
#ifndef MODO_MUDO
#define MODO_MUDO 0   // 1 = bancada: nada sai p/ Discord, Pushbullet ou MQTT
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
    msg += "  " + String(pulsos[i].durMs) + " ms";
    if (pulsos[i].ampMin >= 0) msg += "  amp " + String(pulsos[i].ampMin);
    msg += "\n";
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

// republicacao periodica do link da documentacao (ver coreLoop)
static unsigned long ultimaDocs = 0;
static bool ultimaDocsValido = false;

void sensorPulsou() {
  ultimaLeituraSensor = millis();
  if (sensorSilencioso) {
    sensorSilencioso = false;
    Serial.println("Sensor: voltou a falar");
  }
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

// Caminho da documentacao. Aparece na SISTEMA ONLINE e e
// republicado sozinho a cada DOCS_REPUBLICA_MS: no Discord a
// conversa rola, o link do boot sai de vista em poucos dias, e o
// pior momento para procurar a documentacao e quando a rua cai.
static String documentacao() {
  return String("Documentação do projeto em:\n") + DOCS_URL + "\n";
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

  String payload = "{\"embeds\":[{\"title\":\"" + escapar(titulo) +
                   "\",\"description\":\"" + escapar(descricao) +
                   "\",\"color\":" + String(cor) +
                   ",\"footer\":{\"text\":\"Monitor de Quedas - Antonina-PR\"}}]}";

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

// Faz o envio; se a rede caiu, guarda na fila para depois.
static void publicar(const String& titulo, const String& corpo, int cor) {
  // MODO MUDO: simula em bancada sem notificar ninguem. A mensagem
  // completa sai no serial — da para conferir o texto que SERIA
  // publicado (Inicio/Retorno/Duracao) sem gastar o webhook.
  if (MODO_MUDO) {
    Serial.println();
    Serial.println("=== [MODO MUDO] mensagem NAO enviada ===");
    Serial.println("Titulo: " + titulo);
    Serial.println(corpo);
    Serial.println("=== fim ===");
    return;
  }

  Aviso a;
  a.titulo = titulo;
  a.corpo = corpo;
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
  payload += ",\"em_queda\":" + String(emQueda ? "true" : "false");
  payload += ",\"sinal_saturado\":" + String(sinalSaturado ? "true" : "false");
  payload += ",\"ts\":" + String((unsigned long)relogioAgora());
  payload += ",\"duracao_s\":" + String((unsigned long)(emQueda ? duracaoQuedaMs() / 1000 : 0));
  payload += ",\"ultima_queda_s\":" + String(durUltimaQueda / 1000);
  payload += ",\"fila\":" + String(filaQtd);
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
  json += ",\"em_queda\":" + String(emQueda ? "true" : "false");
  json += ",\"amp\":" + String(ampAtual);
  json += ",\"sinal_saturado\":" + String(sinalSaturado ? "true" : "false");
  json += ",\"desde\":\"" + horaMudanca + "\"";
  json += ",\"duracao_s\":" + String((unsigned long)(emQueda ? duracaoQuedaMs() / 1000 : 0));
  json += ",\"ultima_queda_s\":" + String(durUltimaQueda / 1000);
  json += ",\"fila\":" + String(filaQtd);
  json += ",\"hora_conf\":" + String(relogioConfia() ? "true" : "false");
  json += ",\"uptime_s\":" + String(millis() / 1000);
  json += ",\"wifi_rssi\":" + String(wifiOk ? WiFi.RSSI() : 0);
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
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

  time_t agora = relogioAgora();

  // --- 1a leitura apos o boot ---
  if (!estadoInicialEnviado) {
    estadoInicialEnviado = true;
    estadoAtual = energia;
    emQueda = (energia == "ausente");
    tsQueda = millis();
    tsMudanca = millis();
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
    if (ampAtual >= 0) msg += "Amplitude ZMPT: " + String(ampAtual) + "\n";
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

    String msg = "Energia restabelecida\n";
    msg += "Início da queda: " + (inicio > 0 ? relogioFormatar(inicio) : String("?")) + "\n";
    msg += "Retorno: " + (agora > 0 ? relogioFormatar(agora) : String("?")) + "\n";
    msg += "Duração: " + relogioFormatarDuracao(durUltimaQueda / 1000) + "\n";
    if (ampAtual >= 0) msg += "Amplitude ZMPT: " + String(ampAtual) + "\n";
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
  msg += documentacao();
  msg += identificacao() + agoraStr();
  publicar("SISTEMA ONLINE", msg, 0x00FFFF);   // ciano

  // Daqui a DOCS_REPUBLICA_MS o link volta sozinho para o Discord.
  ultimaDocs = millis();
  ultimaDocsValido = true;

  // 5) servidor HTTP
  servidor.on("/", []() { rotaRaiz(); });
  servidor.on("/estado", []() { rotaEstado(); });
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
    if (!wifiOk) Serial.println("WiFi: voltou");
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

  // --- Checkpoint periodico (mantem o epoch corrente na flash) ---
  static unsigned long ultimoCheckpoint = 0;
  if (millis() - ultimoCheckpoint > CHECKPOINT_MS) {
    ultimoCheckpoint = millis();
    if (estadoInicialEnviado) salvarCheckpoint();
  }

  // --- Republica o link da documentacao a cada DOCS_REPUBLICA_MS ---
  // O Discord rola a conversa: o link do boot sai de vista em
  // poucos dias. Republicar mantem o caminho da documentacao a um
  // clique de distancia o tempo todo.
  if (ultimaDocsValido && millis() - ultimaDocs > DOCS_REPUBLICA_MS) {
    ultimaDocs = millis();
    String msg = documentacao();
    msg += "Republicação periódica — o link acima é enviado a cada 7 dias\n";
    msg += "para não sair da conversa.\n";
    msg += identificacao() + agoraStr();
    Serial.println("Republicando o link da documentacao");
    publicar("DOCUMENTAÇÃO DO PROJETO", msg, 0x00FFFF);   // ciano
  }

  // --- Watchdog: Arduino parou de falar? ---
  // O sensor manda uma linha a cada ~500 ms. Se o Arduino morrer
  // (fio solto, reset em loop, porta USB caiu), o gateway avisa
  // em vez de ficar publicando um estado que nao muda mais.
#if AVISAR_SEM_SENSOR
  if (estadoInicialEnviado && ultimaLeituraSensor > 0 && !sensorSilencioso &&
      millis() - ultimaLeituraSensor > SENSOR_SILENCIO_MS) {
    sensorSilencioso = true;
    String msg = "O Arduino parou de enviar dados ha mais de " +
                 relogioFormatarDuracao(SENSOR_SILENCIO_MS / 1000) + ".\n";
    msg += "O estado exibido pode estar desatualizado.\n";
    msg += "Confira a ligacao UART (TX do Arduino -> divisor -> GPIO16) e\n";
    msg += "se o Arduino continua alimentado.\n\n";
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
    msg += "Detecção: o ZMPT101B do Arduino parou de ver alternada na rede\n";
    if (ampAtual >= 0) msg += "Amplitude ZMPT: " + String(ampAtual) + " (limiar 65)\n";
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

  servidor.handleClient();
  mqttLoop();
}
