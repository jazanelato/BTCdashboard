/*
 * BTCdashboard.ino
 * Mini dashboard de Bitcoin para LILYGO T5 2.13" E-Paper (ESP32)
 * Painel: DEPG0213BN (DKE) - 250x122 em rotacao 1
 * Bibliotecas: GxEPD2 (ZinggJM) + Adafruit GFX - core ESP32 3.x
 *
 * Layout:
 *   Coluna esquerda (0..103)  -> preco atual, variacao 24h e variacao do periodo
 *   Coluna direita  (106..249)-> grafico de linha do periodo selecionado
 *   Rodape (y 105..121)       -> relogio, sinal WiFi e bateria (GPIO35)
 *
 * Botao GPIO39:
 *   clique curto   -> proximo periodo (1 dia, 1 semana, 1 mes, 6 meses, 1 ano, tudo)
 *   segurar >800ms -> recarrega o periodo atual
 *
 * Atualizacao:
 *   preco   -> a cada 60 s  (refresh parcial, so a coluna esquerda)
 *   grafico -> a cada 5 min (refresh completo da tela)
 *
 * Dados: API publica da Binance (sem chave)
 *   /api/v3/ticker/24hr    -> lastPrice + priceChangePercent
 *   /api/v3/klines         -> candles do periodo (campo de fechamento)
 *
 * REGRA DE OURO da GxEPD2: todo desenho vai entre firstPage() e nextPage().
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <time.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>

// ---------- WiFi ----------
// Copie secrets.example.h para secrets.h e preencha com os dados da sua rede.
// O secrets.h esta no .gitignore e nao vai para o repositorio.
#include "secrets.h"

// ---------- Fuso (Brasilia, sem horario de verao) ----------
const long  GMT_OFFSET_S = -3 * 3600;
const int   DST_OFFSET_S = 0;

// ---------- Pinos do T5 v2.3 ----------
#define SPI_MOSI     23
#define SPI_MISO     -1
#define SPI_CLK      18
#define ELINK_SS      5
#define ELINK_BUSY    4
#define ELINK_RESET  16
#define ELINK_DC     17
#define BUTTON_PIN   39

// ---------- Bateria ----------
// O T5 v2.3 leva a bateria a GPIO35 (ADC1_CH7) por um divisor 1:2 (1M/1M).
// ADC1, entao a leitura continua valendo com o WiFi ligado.
// analogReadMilliVolts() ja aplica a calibracao de fabrica gravada na eFuse,
// entao basta multiplicar pelo fator do divisor.
#define BATT_PIN       35
#define BATT_DIV       2.0f    // ajuste fino aqui se o multimetro discordar
#define BATT_AMOSTRAS  16
#define BATT_USB_V     4.25f   // acima disso e' alimentacao externa/carga

// ---------- Painel ----------
#define PANEL GxEPD2_213_BN
GxEPD2_BW<PANEL, PANEL::HEIGHT> display(PANEL(ELINK_SS, ELINK_DC, ELINK_RESET, ELINK_BUSY));

// ---------- Geometria ----------
int W = 250, H = 122;
const int COL_W    = 104;   // largura da coluna esquerda
const int HEAD_H   = 17;    // faixa de titulo
const int FOOT_Y   = 104;   // linha divisoria do rodape
const int CHART_X  = 108;
const int CHART_Y  = 14;
const int CHART_W  = 140;
const int CHART_H  = 86;

// Janela parcial do preco.
// ATENCAO: a GxEPD2 alinha a janela parcial em blocos de 8 px no eixo NATIVO
// do painel. Em rotacao 1 o "y" logico vira o "x" nativo (x_nat = 122 - y - h),
// entao uma janela mal escolhida e' esticada para tras e apaga parte do titulo.
// Para nao esticar: (122 - PART_Y) e PART_H precisam ser multiplos de 8.
//   122 - 18 = 104 (ok)   e   PART_H = 104 (ok)  ->  cobre exatamente y 18..121
const int PART_Y = 18;
const int PART_H = 104;

// ---------- Periodos do grafico (botao GPIO39 alterna) ----------
// A Binance devolve sempre os candles MAIS RECENTES quando so' passamos limit,
// entao limit x interval define a janela de tempo.
struct Period {
  const char* titulo;    // titulo do grafico
  const char* rotulo;    // rotulo curto na coluna esquerda
  const char* interval;  // intervalo do candle na API
  int         limit;     // quantos candles
};
const Period PERIODS[] = {
  { "1 DIA",    "1d",  "5m", 288 },   // 288 x 5min  = 24 h
  { "1 SEMANA", "7d",  "1h", 168 },   // 168 x 1h    = 7 dias
  { "1 MES",    "30d", "4h", 180 },   // 180 x 4h    = 30 dias
  { "6 MESES",  "6m",  "1d", 180 },   // 180 x 1d    = ~6 meses
  { "1 ANO",    "1a",  "1d", 365 },   // 365 x 1d    = 1 ano
  { "TUDO",     "max", "1w", 500 },   // 500 x 1w    = ~9,5 anos (todo o par)
};
const int NPER    = sizeof(PERIODS) / sizeof(PERIODS[0]);
const int PER_INI = 3;               // comeca em 6 MESES
int       per     = PER_INI;

// ---------- Dados ----------
const int MAX_N = 500;           // maior limit da tabela acima
float   hist[MAX_N];
int     histN     = 0;
double  price     = 0;
double  chg24     = 0;
double  chgPer    = 0;           // variacao no periodo selecionado
bool    okPrice   = false;
bool    okHist    = false;
bool    loading   = false;       // mostra "..." enquanto busca

float   battVolts = 0.0f;
uint8_t battPct   = 0;
bool    battUsb   = false;

// =====================================================================
//  Bateria
// =====================================================================

// Curva de descarga de uma celula LiPo em repouso. Interpolar linearmente
// entre 4.2 e 3.3 V erra feio: a celula passa metade da vida entre 3.9 e 3.7 V.
const struct { float v; uint8_t p; } LIPO[] = {
  { 4.20f, 100 }, { 4.10f, 90 }, { 4.00f, 80 }, { 3.93f, 70 },
  { 3.87f,  60 }, { 3.82f, 50 }, { 3.79f, 40 }, { 3.77f, 30 },
  { 3.74f,  20 }, { 3.68f, 10 }, { 3.45f,  5 }, { 3.30f,  0 }
};
const int LIPO_N = sizeof(LIPO) / sizeof(LIPO[0]);

uint8_t lipoPct(float v) {
  if (v >= LIPO[0].v)          return 100;
  if (v <= LIPO[LIPO_N - 1].v) return 0;
  for (int i = 0; i < LIPO_N - 1; i++) {
    if (v <= LIPO[i].v && v > LIPO[i + 1].v) {
      float f = (v - LIPO[i + 1].v) / (LIPO[i].v - LIPO[i + 1].v);
      return LIPO[i + 1].p + f * (LIPO[i].p - LIPO[i + 1].p);
    }
  }
  return 0;
}

// Media de varias leituras: o ADC do ESP32 e ruidoso e uma amostra so oscila
// facil 50 mV, o que na parte plana da curva vale uns 8% de diferenca.
void battLer() {
  uint32_t somaMv = 0;
  for (int i = 0; i < BATT_AMOSTRAS; i++) {
    somaMv += analogReadMilliVolts(BATT_PIN);
    delay(2);
  }
  battVolts = (somaMv / (float)BATT_AMOSTRAS) / 1000.0f * BATT_DIV;
  battUsb   = (battVolts > BATT_USB_V);   // heuristica: no USB o pino fica ~4.3+
  battPct   = lipoPct(battVolts);
  Serial.printf("bateria %.2f V -> %d%%%s\n", battVolts, battPct, battUsb ? " (USB)" : "");
}

// ---------- Temporizacao ----------
const uint32_t PRICE_MS = 60UL  * 1000UL;   // 1 min
const uint32_t CHART_MS = 300UL * 1000UL;   // 5 min
uint32_t tPrice = 0, tChart = 0;

// =====================================================================
//  Helpers de tela
// =====================================================================
void beginFull() {
  display.setFullWindow();
  display.firstPage();
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
}

void beginPartial(int x, int y, int w, int h) {
  display.setPartialWindow(x, y, w, h);
  display.firstPage();
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
}

uint16_t textW(const char* s) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Texto na fonte classica 5x7 (cursor = canto superior esquerdo)
void small(int x, int y, const char* s) {
  display.setFont(NULL);
  display.setTextSize(1);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(x, y);
  display.print(s);
}

void smallRight(int xr, int y, const char* s) {
  small(xr - (int)strlen(s) * 6, y, s);
}

// Icone de pilha deitada, com o nivel proporcional a pct.
// A cor vem de fora para o icone servir tanto em fundo branco quanto na
// faixa preta do cabecalho.
void iconeBateria(int x, int y, int w, int h, uint8_t pct, uint16_t cor) {
  int corpo = w - 3;                       // sobra 3 px para o polo positivo
  display.drawRect(x, y, corpo, h, cor);
  display.fillRect(x + corpo, y + h / 3, 3, h - 2 * (h / 3), cor);

  int util  = corpo - 4;
  int cheio = (util * (pct > 100 ? 100 : pct)) / 100;
  if (cheio > 0) display.fillRect(x + 2, y + 2, cheio, h - 4, cor);
}

// Rotulo pequeno com fundo branco (para escrever por cima do grafico)
void labelBox(int x, int y, const char* s) {
  int w = (int)strlen(s) * 6;
  display.fillRect(x - 1, y - 1, w + 2, 9, GxEPD_WHITE);
  small(x, y, s);
}

// =====================================================================
//  Formatacao
// =====================================================================
void fmtPrice(double v, char* out, size_t n) {
  long i = (long)(v + 0.5);
  if (i < 0) i = 0;
  if (i >= 1000000L) snprintf(out, n, "%ld,%03ld,%03ld", i / 1000000L, (i / 1000L) % 1000L, i % 1000L);
  else if (i >= 1000L) snprintf(out, n, "%ld,%03ld", i / 1000L, i % 1000L);
  else                 snprintf(out, n, "%ld", i);
}

void fmtPct(double v, char* out, size_t n) {
  snprintf(out, n, "%+.2f%%", v);
}

void fmtK(float v, char* out, size_t n) {
  if (v >= 1000.0f) snprintf(out, n, "%.1fk", v / 1000.0f);
  else              snprintf(out, n, "%.0f", v);
}

void fmtClock(char* out, size_t n) {
  struct tm t;
  if (getLocalTime(&t, 50)) strftime(out, n, "%H:%M", &t);
  else                      snprintf(out, n, "--:--");
}

// =====================================================================
//  Rede
// =====================================================================
bool wifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ok" : " falhou");
  return WiFi.status() == WL_CONNECTED;
}

// Extrai o valor de "chave": de um JSON simples (string ou numero)
String jsonField(const String& src, const char* key) {
  String k = String("\"") + key + "\":";
  int i = src.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  if (i < (int)src.length() && src[i] == '"') {
    i++;
    int j = src.indexOf('"', i);
    if (j < 0) return "";
    return src.substring(i, j);
  }
  int j = i;
  while (j < (int)src.length() && src[j] != ',' && src[j] != '}') j++;
  return src.substring(i, j);
}

// Preco atual + variacao de 24 h
bool fetchTicker() {
  if (!wifiUp()) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, "https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT")) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("ticker HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  String p = jsonField(body, "lastPrice");
  String c = jsonField(body, "priceChangePercent");
  if (p.length() == 0) return false;

  price = p.toDouble();
  chg24 = c.toDouble();
  if (histN > 1 && hist[0] > 0) chgPer = (price - hist[0]) / hist[0] * 100.0;
  Serial.printf("preco %.2f  24h %.2f%%\n", price, chg24);
  return true;
}

// Candles do periodo atual; le o stream e captura so o campo 4 (fechamento),
// evitando alocar o JSON inteiro na RAM
bool fetchKlines() {
  if (!wifiUp()) return false;
  const Period& P = PERIODS[per];
  const int lim = (P.limit < MAX_N) ? P.limit : MAX_N;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  String url = String("https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=")
             + P.interval + "&limit=" + String(lim);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("klines HTTP %d\n", code);
    http.end();
    return false;
  }

  WiFiClient* s = http.getStreamPtr();
  int  n = 0, depth = 0, field = 0, bi = 0;
  bool capture = false;
  char buf[24];
  uint32_t t0 = millis();

  while (n < lim && millis() - t0 < 25000) {
    if (!s->available()) {
      if (!http.connected()) break;
      delay(2);
      continue;
    }
    char c = (char)s->read();

    if (depth == 2) {
      if (capture) {
        if (c == '"') { buf[bi] = 0; capture = false; hist[n] = atof(buf); }
        else if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
        continue;
      }
      if (c == '"' && field == 4) { capture = true; bi = 0; continue; }
      if (c == ',') { field++; continue; }
      if (c == ']') { depth = 1; n++; continue; }
      continue;
    }
    if (c == '[') { depth++; if (depth == 2) field = 0; }
    else if (c == ']' && depth > 0) depth--;
  }
  http.end();

  if (n < 10) { Serial.printf("klines incompleto (%d)\n", n); return false; }
  histN = n;
  if (price > 0 && hist[0] > 0) chgPer = (price - hist[0]) / hist[0] * 100.0;
  Serial.printf("historico %s: %d candles de %s (%.0f -> %.0f)\n",
                P.titulo, histN, P.interval, hist[0], hist[histN - 1]);
  return true;
}

// =====================================================================
//  Desenho
// =====================================================================

// Faixa de titulo da coluna esquerda, com a moeda BTC
void drawHeader() {
  display.fillRect(0, 0, COL_W, HEAD_H, GxEPD_BLACK);
  display.fillCircle(9, 8, 7, GxEPD_WHITE);
  small(7, 5, "B");
  display.drawFastVLine(8, 3, 11, GxEPD_BLACK);
  display.drawFastVLine(11, 3, 11, GxEPD_BLACK);

  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(20, 13);
  display.print("BTC/USD");
  display.setTextColor(GxEPD_BLACK);
}

// Preco, variacoes e relogio (tudo que muda a cada minuto)
void drawLeftBody() {
  char b[24];

  // Preco: 12 pt e, se ficar largo demais, cai para 9 pt
  fmtPrice(price, b, sizeof(b));
  display.setFont(&FreeMonoBold12pt7b);
  if (textW(b) > COL_W - 4) display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(2, 45);
  display.print(okPrice ? b : "--");

  // Variacao 24 h
  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(2, 68);
  if (okPrice) { fmtPct(chg24, b, sizeof(b)); display.print(b); }
  else display.print("--");
  smallRight(COL_W - 3, 62, "24h");

  // Variacao no periodo selecionado
  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(2, 90);
  if (loading)                   display.print("...");
  else if (okHist && okPrice)  { fmtPct(chgPer, b, sizeof(b)); display.print(b); }
  else                           display.print("--");
  smallRight(COL_W - 3, 84, PERIODS[per].rotulo);

  // Relogio no rodape esquerdo
  display.drawFastHLine(0, FOOT_Y, COL_W, GxEPD_BLACK);
  fmtClock(b, sizeof(b));
  small(2, 110, b);
}

void drawChart() {
  const int x = CHART_X, y = CHART_Y, w = CHART_W, h = CHART_H;

  char t[28];
  snprintf(t, sizeof(t), "%s %dx%s", PERIODS[per].titulo, histN, PERIODS[per].interval);
  small(CHART_X, 3, okHist ? t : PERIODS[per].titulo);

  // Indicador de posicao: um quadradinho por periodo, o atual preenchido
  for (int k = 0; k < NPER; k++) {
    int bx = W - 2 - (NPER - k) * 7;
    if (k == per) display.fillRect(bx, 3, 5, 5, GxEPD_BLACK);
    else          display.drawRect(bx, 3, 5, 5, GxEPD_BLACK);
  }

  display.drawRect(x, y, w, h, GxEPD_BLACK);

  if (!okHist || histN < 2) {
    small(x + 30, y + h / 2 - 3, "sem dados");
    return;
  }

  float mn = hist[0], mx = hist[0];
  for (int i = 1; i < histN; i++) {
    if (hist[i] < mn) mn = hist[i];
    if (hist[i] > mx) mx = hist[i];
  }
  if (mx - mn < 1.0f) mx = mn + 1.0f;

  // Grade pontilhada em 1/4, 1/2 e 3/4
  for (int k = 1; k <= 3; k++) {
    int gy = y + (h * k) / 4;
    for (int px = x + 2; px < x + w - 2; px += 4) display.drawPixel(px, gy, GxEPD_BLACK);
  }

  const int ix = x + 1, iy = y + 1, iw = w - 2, ih = h - 2;
  int prevX = 0, prevY = 0;
  for (int px = 0; px < iw; px++) {
    int idx = (int)(((long)px * (histN - 1)) / (iw - 1));
    int py  = iy + ih - 1 - (int)(((hist[idx] - mn) / (mx - mn)) * (ih - 1) + 0.5f);
    int cx  = ix + px;
    if (px > 0) {
      display.drawLine(prevX, prevY, cx, py, GxEPD_BLACK);      // traco de 2 px
      display.drawLine(prevX, prevY + 1, cx, py + 1, GxEPD_BLACK);
    }
    prevX = cx; prevY = py;
  }
  display.fillCircle(prevX, prevY, 2, GxEPD_BLACK);

  char b[12];
  fmtK(mx, b, sizeof(b)); labelBox(x + 3, y + 3, b);
  fmtK(mn, b, sizeof(b)); labelBox(x + 3, y + h - 11, b);
}

void drawFooterRight() {
  char b[32];
  display.drawFastHLine(COL_W, FOOT_Y, W - COL_W, GxEPD_BLACK);

  // Sinal do WiFi (RSSI em dBm: -50 otimo, -70 fraco, -85 comeca a cair)
  if (WiFi.status() == WL_CONNECTED) snprintf(b, sizeof(b), "%ddBm", WiFi.RSSI());
  else                               snprintf(b, sizeof(b), "sem wifi");
  small(CHART_X, 110, b);

  // Bateria no canto inferior direito: porcentagem + pilha
  const int ICO_W = 24, ICO_H = 9, ICO_X = W - 2 - ICO_W, ICO_Y = 108;
  iconeBateria(ICO_X, ICO_Y, ICO_W, ICO_H, battPct, GxEPD_BLACK);
  if (battUsb) snprintf(b, sizeof(b), "USB");
  else         snprintf(b, sizeof(b), "%d%%", battPct);
  smallRight(ICO_X - 4, 110, b);
}

// Tela inteira
void drawAll() {
  beginFull();
  drawHeader();
  drawLeftBody();
  display.drawFastVLine(COL_W, 0, FOOT_Y, GxEPD_BLACK);
  drawChart();
  drawFooterRight();
  uint32_t t = millis();
  display.nextPage();
  Serial.printf("full refresh: %lu ms\n", millis() - t);
}

// So a coluna esquerda (preco/variacoes/relogio)
void drawPriceOnly() {
  beginPartial(0, PART_Y, COL_W, PART_H);
  drawLeftBody();
  // a divisoria em x=COL_W fica fora da janela: nao e' apagada nem redesenhada
  uint32_t t = millis();
  display.nextPage();
  display.setFullWindow();
  Serial.printf("partial refresh: %lu ms\n", millis() - t);
}

// =====================================================================
//  Ciclos
// =====================================================================
void updatePrice(bool full) {
  okPrice = fetchTicker();
  battLer();
  if (full) drawAll(); else drawPriceOnly();
  tPrice = millis();
}

void updateChart() {
  okHist  = fetchKlines();
  okPrice = fetchTicker();
  battLer();
  drawAll();
  tChart = millis();
  tPrice = millis();
}

// Troca de periodo: da um refresh parcial rapido so' para o mostrador nao
// ficar "morto" enquanto a requisicao (2-5 s) acontece, depois redesenha tudo
void aplicaPeriodo() {
  loading = true;
  okHist  = false;
  histN   = 0;
  drawPriceOnly();
  loading = false;
  updateChart();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== BTC dashboard - T5 2.13\" / GxEPD2 ===");

  pinMode(BUTTON_PIN, INPUT);

  // ADC da bateria: 11 dB cobre a faixa ate' ~2.5 V no pino (5 V na bateria)
  analogSetPinAttenuation(BATT_PIN, ADC_11db);
  battLer();

  // A GxEPD2 abre o SPI com os pinos padrao; refazemos com os da placa
  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI, ELINK_SS);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
  W = display.width();
  H = display.height();

  // Tela de abertura enquanto os dados nao chegam
  beginFull();
  drawHeader();
  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(4, 60);
  display.print("conectando");
  display.setCursor(4, 80);
  display.print("...");
  display.nextPage();

  wifiUp();
  configTime(GMT_OFFSET_S, DST_OFFSET_S, "pool.ntp.org", "a.st1.ntp.br");
  updateChart();
}

void loop() {
  // Botao GPIO39 (ativo em nivel baixo):
  //   clique curto  -> proximo periodo do grafico
  //   segurar >800ms-> so' recarrega o periodo atual
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(40);                                  // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      uint32_t t0 = millis();
      while (digitalRead(BUTTON_PIN) == LOW && millis() - t0 < 3000) delay(10);
      uint32_t held = millis() - t0;
      if (held < 800) per = (per + 1) % NPER;
      Serial.printf("botao (%lu ms) -> %s\n", held, PERIODS[per].titulo);
      aplicaPeriodo();
      return;
    }
  }

  uint32_t now = millis();
  if (now - tChart >= CHART_MS)      updateChart();
  else if (now - tPrice >= PRICE_MS) updatePrice(false);

  delay(50);
}
