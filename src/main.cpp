// ============================================================================
//  Small TV Evolution Pro  -  firmware per NM-TV-154 (ESP32-WROOM-32E)
//
//  Menu di schermate comandato dal pulsante touch superiore (GPIO32 / T9)
//  oppure dai tre tasti dell'interfaccia web:
//    MENU   : 1 tocco = avanti, 2 tocchi = indietro, tieni premuto = entra
//    DENTRO : 1 tocco = funzione successiva, 2 tocchi = funzione precedente,
//             tieni premuto = torna al menu
//    Tieni premuto 8 secondi = portale di configurazione WiFi/citta'
//
//  Meteo : principale | prossimi 3 giorni | grafico temperatura
//  Aria  : indice EAQI | salute (UV e pollini)
//  Dati: Open-Meteo (gratuito, nessuna chiave API)
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <math.h>

#define FW_VERSION "1.0.4"

// ---------------------------------------------------------------- Pin -------
#define PIN_BL        19   // retroilluminazione, PWM, attiva bassa
#define PIN_PANEL_PWR 21   // alimentazione pannello, va portato a LOW
#define PIN_TOUCH     32   // pulsante touch (canale T9)
#define AP_NAME       "NM-TV-Setup"

// ------------------------------------------------------------- Colori -------
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static inline uint16_t c565(uint32_t rgb) {
  return RGB565((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

// t = 0 -> a, t = 255 -> b
uint32_t mixRGB(uint32_t a, uint32_t b, uint8_t t) {
  uint32_t r  = (((a >> 16) & 255) * (255 - t) + ((b >> 16) & 255) * t) / 255;
  uint32_t g  = (((a >> 8) & 255) * (255 - t) + ((b >> 8) & 255) * t) / 255;
  uint32_t bl = ((a & 255) * (255 - t) + (b & 255) * t) / 255;
  return (r << 16) | (g << 8) | bl;
}

// colori fissi (icone meteo e segnali): non dipendono dal tema
static const uint16_t C_SUN    = RGB565(255, 196, 40);
static const uint16_t C_CLOUD  = RGB565(206, 215, 232);
static const uint16_t C_CLOUDD = RGB565(120, 132, 156);
static const uint16_t C_RAIN   = RGB565(70, 150, 255);
static const uint16_t C_SNOW   = RGB565(235, 243, 255);
static const uint16_t C_BOLT   = RGB565(255, 225, 60);
static const uint16_t C_MAX    = RGB565(255, 150, 60);
static const uint16_t C_MIN    = RGB565(110, 190, 255);
static const uint16_t C_MOON   = RGB565(236, 236, 205);
static const uint16_t C_ERR    = RGB565(255, 110, 110);
static const uint16_t C_AIR    = RGB565(80, 220, 200);

// Temi (0xRRGGBB), gli stessi di SmallTV Evolution. Il primo (Neon) e' quello di default.
struct Theme {
  uint32_t bg, card, a1, a2, text, dim, band;
};
const Theme THEMES[] = {
  // bg        card      a1        a2        text      dim       band
  {0x05060F, 0x0E1230, 0x00E5FF, 0xFF2E93, 0xFFFFFF, 0x6B7399, 0x2A1470},  // Neon
  {0x030F0E, 0x0A2422, 0x2DFFB2, 0x7C5CFF, 0xEFFFFA, 0x5F8F86, 0x0B4A48},  // Aurora
  {0x120610, 0x2A0F26, 0xFF9F1C, 0xFF3D81, 0xFFF4E6, 0x9C6B7E, 0x6A1A4A},  // Tramonto
  {0x06101C, 0x0F2338, 0x7FD1FF, 0xA78BFA, 0xF2F8FF, 0x6F8CAB, 0x14487A},  // Ghiaccio
  {0x000000, 0x14100A, 0xFFB000, 0xFF6A00, 0xFFE9B8, 0x7A5C1E, 0x4A3400},  // Ambra
};
const uint8_t THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
static const char* const THEME_NAME[5] = {"Neon (cyberpunk)", "Aurora", "Tramonto", "Ghiaccio", "Ambra"};
const Theme* th = &THEMES[0];

// colori del tema in uso (si impostano con applyTheme)
uint16_t C_BG = 0, C_CARD = 0, C_TRACK = 0, C_TEXT = 0xFFFF, C_DIM = 0, C_ACC = 0, C_ACC2 = 0;

void applyTheme(uint8_t i) {
  if (i >= THEME_COUNT) i = 0;
  th = &THEMES[i];
  C_BG    = c565(th->bg);
  C_CARD  = c565(th->card);
  C_TRACK = c565(mixRGB(th->card, th->dim, 90));
  C_TEXT  = c565(th->text);
  C_DIM   = c565(th->dim);
  C_ACC   = c565(th->a1);
  C_ACC2  = c565(th->a2);
}

// Fasce EAQI (indice europeo): 0-20, 20-40, 40-60, 60-80, 80-100, >100
static const uint16_t BANDCOL[6] = {
  RGB565(80, 240, 230), RGB565(80, 204, 170), RGB565(240, 230, 65),
  RGB565(255, 80, 80),  RGB565(160, 0, 60),   RGB565(135, 40, 140)
};
static const char* BANDTXT[6] = {"Ottima", "Buona", "Discreta", "Scadente", "Molto scadente", "Pessima"};

static const char* GIORNI[7] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
static const char* MESI[12]  = {"Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno",
                                "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre"};

static const char* TZ_ROME = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Rome, ora legale automatica

static const uint32_t WEATHER_MS = 15UL * 60UL * 1000UL;     // meteo ogni 15 minuti
static const uint32_t AIR_MS     = 30UL * 60UL * 1000UL;     // aria ogni 30 minuti

// ------------------------------------------------------- Configurazione -----
struct Config {
  String ssid, pass, cityq, city;
  float lat = 0, lon = 0;
  bool geo = false, fmt12 = false, inv = true, rot = false;
  uint8_t theme = 0;
};
Config cfg;
Preferences prefs;

void loadCfg() {
  prefs.begin("nmtv", true);
  cfg.ssid  = prefs.getString("ssid", "");
  cfg.pass  = prefs.getString("pass", "");
  cfg.cityq = prefs.getString("cityq", "Roma");
  cfg.city  = prefs.getString("city", "");
  cfg.lat   = prefs.getFloat("lat", 0);
  cfg.lon   = prefs.getFloat("lon", 0);
  cfg.geo   = prefs.getBool("geo", false);
  cfg.fmt12 = prefs.getBool("fmt12", false);
  cfg.inv   = prefs.getBool("inv", true);
  cfg.rot   = prefs.getBool("rot", false);
  cfg.theme = prefs.getUChar("theme", 0);
  prefs.end();
  if (cfg.theme >= THEME_COUNT) cfg.theme = 0;
}

void saveCfg() {
  prefs.begin("nmtv", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("cityq", cfg.cityq);
  prefs.putString("city", cfg.city);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putBool("geo", cfg.geo);
  prefs.putBool("fmt12", cfg.fmt12);
  prefs.putBool("inv", cfg.inv);
  prefs.putBool("rot", cfg.rot);
  prefs.putUChar("theme", cfg.theme);
  prefs.end();
}

// ----------------------------------------------------------- Globali --------
TFT_eSPI tft = TFT_eSPI();
WebServer server(80);
DNSServer dns;
String netOpts;   // elenco reti WiFi (solo in modalita' portale)

// ------------------------------------------------------- Dati condivisi -----
struct Weather {
  bool valid = false;
  float t = 0, feels = 0, hum = 0, press = 0, wind = 0;
  int code = 0;
  bool day = true;
  float tmin[4] = {0, 0, 0, 0};
  float tmax[4] = {0, 0, 0, 0};
  int dcode[4] = {0, 0, 0, 0};
  String dday[4];
  int srH = 0, srM = 0, ssH = 0, ssM = 0;
  float hh[24];            // temperatura oraria, prime 24 ore a partire da adesso
  bool hvalid = false;
  int hstart = 0;          // ora del giorno del primo valore
  time_t upd = 0;
};
struct Air {
  bool valid = false;
  float aqi = 0, pm25 = 0, pm10 = 0;
  float uv = 0, uvMax = 0;               // indice UV attuale e massimo previsto oggi
  float pGrass = 0, pOlive = 0, pBirch = 0;   // pollini in granuli/m3
  bool hasPollen = false;
  time_t upd = 0;
};

enum { ST_WIFI, ST_GEO, ST_FETCH, ST_OK, ST_ERR_GEO, ST_ERR_NET };

SemaphoreHandle_t mtx;
Weather Wsh, W;          // Wsh = scritto dal task dati, W = copia per il display
Air Ash, A;
volatile uint32_t gVer = 0;
volatile int gStatus = ST_WIFI;
volatile bool gPortal = false;   // vero durante il portale o un aggiornamento: il task dati si ferma
uint32_t seenVer = 0;

void setStatus(int s) {
  if (gStatus != s) {
    gStatus = s;
    gVer = gVer + 1;
  }
}

const char* statusText() {
  switch (gStatus) {
    case ST_WIFI:    return "Connessione WiFi...";
    case ST_GEO:     return "Ricerca citta...";
    case ST_FETCH:   return "Aggiorno i dati...";
    case ST_ERR_GEO: return "Citta non trovata";
    case ST_ERR_NET: return "Errore di rete";
    default:         return "";
  }
}

bool syncData() {
  if (gVer == seenVer) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  W = Wsh;
  A = Ash;
  seenVer = gVer;
  xSemaphoreGive(mtx);
  return true;
}

// ----------------------------------------------------------- Utilita' -------
String urlEnc(const String& s) {
  String o;
  char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += (char)c;
    } else {
      snprintf(b, sizeof b, "%%%02X", c);
      o += b;
    }
  }
  return o;
}

String htmlEsc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

bool getTm(struct tm& t) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &t);
  return true;
}

String fmtHM(int h, int m) {
  char b[16];
  if (cfg.fmt12) {
    int hh = h % 12;
    if (hh == 0) hh = 12;
    snprintf(b, sizeof b, "%d:%02d %s", hh, m, h >= 12 ? "PM" : "AM");
  } else {
    snprintf(b, sizeof b, "%02d:%02d", h, m);
  }
  return String(b);
}

String hourLabel(int h) {
  char b[8];
  if (cfg.fmt12) {
    int hh = h % 12;
    if (hh == 0) hh = 12;
    snprintf(b, sizeof b, "%d%s", hh, h >= 12 ? "PM" : "AM");
  } else {
    snprintf(b, sizeof b, "%02d", h);
  }
  return String(b);
}

int dowOf(int y, int m, int d) {   // 0 = domenica
  static const int tt[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + tt[m - 1] + d) % 7;
}

// ------------------------------------------------------------- Rete ---------
bool getJson(const String& url, JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  return deserializeJson(doc, body) == DeserializationError::Ok;
}

// 0 = ok, 1 = errore di rete, 2 = citta' non trovata
int geocode() {
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" + urlEnc(cfg.cityq) +
               "&count=1&language=it&format=json";
  JsonDocument doc;
  if (!getJson(url, doc)) return 1;
  JsonArray r = doc["results"];
  if (r.isNull() || r.size() == 0) return 2;
  cfg.lat = r[0]["latitude"] | 0.0f;
  cfg.lon = r[0]["longitude"] | 0.0f;
  String nm = r[0]["name"].as<String>();
  cfg.city = nm.length() ? nm : cfg.cityq;
  cfg.geo = true;
  saveCfg();
  return 0;
}

bool fetchWeather() {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
               "weather_code,pressure_msl,wind_speed_10m"
               "&hourly=temperature_2m"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
               "&timezone=Europe%2FRome&forecast_days=4&wind_speed_unit=kmh";
  JsonDocument doc;
  if (!getJson(url, doc)) return false;
  JsonObject cur = doc["current"];
  JsonObject d = doc["daily"];
  if (cur.isNull() || d.isNull()) return false;

  Weather w;
  w.valid = true;
  w.t     = cur["temperature_2m"] | 0.0f;
  w.feels = cur["apparent_temperature"] | 0.0f;
  w.hum   = cur["relative_humidity_2m"] | 0.0f;
  w.press = cur["pressure_msl"] | 0.0f;
  w.wind  = cur["wind_speed_10m"] | 0.0f;
  w.code  = cur["weather_code"] | 0;
  w.day   = (cur["is_day"] | 1) == 1;
  for (int i = 0; i < 4; i++) {
    w.dcode[i] = d["weather_code"][i] | 0;
    w.tmax[i]  = d["temperature_2m_max"][i] | 0.0f;
    w.tmin[i]  = d["temperature_2m_min"][i] | 0.0f;
    w.dday[i]  = d["time"][i].as<String>();
  }
  String sr = d["sunrise"][0].as<String>();   // "2026-11-15T06:55"
  String ss = d["sunset"][0].as<String>();
  if (sr.length() >= 16) { w.srH = sr.substring(11, 13).toInt(); w.srM = sr.substring(14, 16).toInt(); }
  if (ss.length() >= 16) { w.ssH = ss.substring(11, 13).toInt(); w.ssM = ss.substring(14, 16).toInt(); }

  // temperatura oraria: l'array parte da oggi alle 00:00, la prima ora utile e' quella attuale
  for (int i = 0; i < 24; i++) w.hh[i] = 0;
  String ct = cur["time"].as<String>();       // "2026-11-16T15:30"
  int h0 = (ct.length() >= 13) ? ct.substring(11, 13).toInt() : 0;
  if (h0 < 0 || h0 > 23) h0 = 0;
  JsonArray ha = doc["hourly"]["temperature_2m"];
  if (!ha.isNull() && (int)ha.size() >= h0 + 24) {
    for (int i = 0; i < 24; i++) w.hh[i] = ha[h0 + i] | 0.0f;
    w.hvalid = true;
    w.hstart = h0;
  }

  time_t n = time(nullptr);
  w.upd = (n > 1700000000) ? n : 0;

  xSemaphoreTake(mtx, portMAX_DELAY);
  Wsh = w;
  gVer = gVer + 1;
  xSemaphoreGive(mtx);
  return true;
}

bool fetchAir() {
  String url = "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&current=european_aqi,pm10,pm2_5,uv_index,grass_pollen,olive_pollen,birch_pollen"
               "&daily=uv_index_max&forecast_days=1&timezone=Europe%2FRome";
  JsonDocument doc;
  if (!getJson(url, doc)) return false;
  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;

  Air a;
  a.valid = true;
  a.aqi  = cur["european_aqi"] | 0.0f;
  a.pm25 = cur["pm2_5"] | 0.0f;
  a.pm10 = cur["pm10"] | 0.0f;
  a.uv    = cur["uv_index"] | 0.0f;
  a.uvMax = doc["daily"]["uv_index_max"][0] | 0.0f;
  a.hasPollen = !(cur["grass_pollen"].isNull() && cur["olive_pollen"].isNull() && cur["birch_pollen"].isNull());
  a.pGrass = cur["grass_pollen"] | 0.0f;
  a.pOlive = cur["olive_pollen"] | 0.0f;
  a.pBirch = cur["birch_pollen"] | 0.0f;
  time_t n = time(nullptr);
  a.upd = (n > 1700000000) ? n : 0;

  xSemaphoreTake(mtx, portMAX_DELAY);
  Ash = a;
  gVer = gVer + 1;
  xSemaphoreGive(mtx);
  return true;
}

void dataTask(void*) {
  uint32_t nextW = 0, nextA = 0, lastTry = millis();
  for (;;) {
    if (gPortal) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      setStatus(ST_WIFI);
      if (millis() - lastTry > 60000UL) {
        lastTry = millis();
        WiFi.disconnect();
        WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
      }
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }
    lastTry = millis();

    if (!cfg.geo) {
      setStatus(ST_GEO);
      int r = geocode();
      if (r == 0) {
        setStatus(ST_FETCH);
        nextW = 0;
        nextA = 0;
      } else {
        setStatus(r == 2 ? ST_ERR_GEO : ST_ERR_NET);
        vTaskDelay(30000 / portTICK_PERIOD_MS);
        continue;
      }
    }

    bool failed = false;
    if ((int32_t)(millis() - nextW) >= 0) {
      bool ok = fetchWeather();
      nextW = millis() + (ok ? WEATHER_MS : 60000UL);
      if (!ok) failed = true;
    }
    if ((int32_t)(millis() - nextA) >= 0) {
      bool ok = fetchAir();
      nextA = millis() + (ok ? AIR_MS : 60000UL);
      if (!ok) failed = true;
    }
    setStatus(failed ? ST_ERR_NET : ST_OK);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ------------------------------------------------------ Testo sul display ---
static int degW(int font) { return font == 4 ? 10 : 7; }

int strW(const String& s, int font) {
  int w = 0;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '~') w += degW(font);
    else w += tft.textWidth(String(c), font);
  }
  return w;
}

// Disegna testo (font 2 o 4). Il carattere '~' diventa il simbolo del grado.
// align: 0 = sinistra, 1 = centro, 2 = destra. Ritorna la larghezza disegnata.
int drawStr(const String& s, int x, int y, uint8_t font, uint8_t align, uint16_t fg, uint16_t bg) {
  tft.setTextColor(fg, bg);
  int w = strW(s, font);
  if (align == 1) x -= w / 2;
  else if (align == 2) x -= w;
  int fh = tft.fontHeight(font);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '~') {
      int r = (font == 4) ? 3 : 2;
      tft.fillRect(x, y, degW(font), fh, bg);
      tft.drawCircle(x + r + 2, y + (font == 4 ? 6 : 4), r, fg);
      if (font == 4) tft.drawCircle(x + r + 2, y + 6, r - 1, fg);
      x += degW(font);
    } else {
      x += tft.drawChar((uint16_t)c, x, y, font);
    }
  }
  return w;
}

// ------------------------------------------------------------ Icone ---------
enum { IC_CLEAR, IC_PARTLY, IC_CLOUD, IC_FOG, IC_DRIZZLE, IC_RAIN, IC_SNOW, IC_STORM };

static inline int S(float v, float k) { return (int)lroundf(v * k); }

void iconSun(int cx, int cy, float k) {
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785398f;
    float ca = cosf(a), sa = sinf(a);
    int x0 = cx + (int)lroundf(ca * S(17, k)), y0 = cy + (int)lroundf(sa * S(17, k));
    int x1 = cx + (int)lroundf(ca * S(25, k)), y1 = cy + (int)lroundf(sa * S(25, k));
    tft.drawLine(x0, y0, x1, y1, C_SUN);
    tft.drawLine(x0 + 1, y0, x1 + 1, y1, C_SUN);
    tft.drawLine(x0, y0 + 1, x1, y1 + 1, C_SUN);
  }
  tft.fillCircle(cx, cy, S(12, k), C_SUN);
}

void iconMoon(int cx, int cy, float k, uint16_t bg) {
  tft.fillCircle(cx, cy, S(16, k), C_MOON);
  tft.fillCircle(cx + S(8, k), cy - S(5, k), S(14, k), bg);
}

void cloudShape(int cx, int cy, float k, uint16_t col) {
  tft.fillCircle(cx - S(14, k), cy + S(4, k), S(10, k), col);
  tft.fillCircle(cx, cy - S(5, k), S(14, k), col);
  tft.fillCircle(cx + S(15, k), cy + S(3, k), S(11, k), col);
  tft.fillRect(cx - S(14, k), cy + S(4, k), S(29, k), S(10, k), col);
}

void drops(int cx, int cy, float k, int len) {
  for (int i = 0; i < 3; i++) {
    int x = cx - S(12, k) + i * S(12, k);
    int y = cy + S(10, k) + (i % 2) * S(4, k);
    tft.drawLine(x, y, x - S(3, k), y + S(len, k), C_RAIN);
    tft.drawLine(x + 1, y, x - S(3, k) + 1, y + S(len, k), C_RAIN);
  }
}

void drawIcon(int cx, int cy, int type, bool day, float k, uint16_t bg) {
  switch (type) {
    case IC_CLEAR:
      if (day) iconSun(cx, cy, k * 1.05f);
      else iconMoon(cx, cy, k * 1.1f, bg);
      break;
    case IC_PARTLY:
      if (day) iconSun(cx - S(10, k), cy - S(10, k), k * 0.75f);
      else iconMoon(cx - S(10, k), cy - S(10, k), k * 0.8f, bg);
      cloudShape(cx + S(6, k), cy + S(8, k), k * 0.85f, C_CLOUD);
      break;
    case IC_CLOUD:
      cloudShape(cx, cy, k * 1.05f, C_CLOUD);
      break;
    case IC_FOG:
      cloudShape(cx, cy - S(10, k), k * 0.9f, C_CLOUDD);
      for (int i = 0; i < 3; i++) {
        int w = S(44, k) - (i % 2) * S(10, k);
        tft.fillRoundRect(cx - w / 2, cy + S(8, k) + i * S(7, k), w, S(3, k) + 1, 1, C_CLOUD);
      }
      break;
    case IC_DRIZZLE:
      cloudShape(cx, cy - S(8, k), k * 0.9f, C_CLOUD);
      drops(cx, cy - S(2, k), k * 0.9f, 5);
      break;
    case IC_RAIN:
      cloudShape(cx, cy - S(8, k), k * 0.9f, C_CLOUDD);
      drops(cx, cy - S(2, k), k * 0.9f, 10);
      break;
    case IC_SNOW:
      cloudShape(cx, cy - S(8, k), k * 0.9f, C_CLOUD);
      for (int i = 0; i < 3; i++) {
        int x = cx - S(12, k) + i * S(12, k);
        int y = cy + S(14, k) + (i % 2) * S(6, k);
        tft.fillCircle(x, y, S(2.6f, k) + 1, C_SNOW);
      }
      break;
    case IC_STORM:
      cloudShape(cx, cy - S(10, k), k * 0.9f, C_CLOUDD);
      tft.fillTriangle(cx + S(5, k), cy + S(2, k), cx - S(6, k), cy + S(16, k), cx + S(1, k), cy + S(16, k), C_BOLT);
      tft.fillTriangle(cx + S(1, k), cy + S(12, k), cx - S(3, k), cy + S(28, k), cx + S(9, k), cy + S(10, k), C_BOLT);
      break;
  }
}

void iconWind(int cx, int cy, float k, uint16_t c1, uint16_t c2, uint16_t bg) {
  // riga alta con ricciolo
  tft.fillRoundRect(cx - S(46, k), cy - S(14, k), S(64, k), S(6, k), S(3, k), c1);
  tft.fillCircle(cx + S(18, k), cy - S(23, k), S(11, k), c1);
  tft.fillCircle(cx + S(18, k), cy - S(23, k), S(6, k), bg);
  tft.fillRect(cx + S(6, k), cy - S(23, k), S(12, k), S(8, k), bg);
  // riga centrale con ricciolo
  tft.fillRoundRect(cx - S(58, k), cy + S(4, k), S(98, k), S(6, k), S(3, k), c2);
  tft.fillCircle(cx + S(40, k), cy - S(6, k), S(10, k), c2);
  tft.fillCircle(cx + S(40, k), cy - S(6, k), S(5, k), bg);
  tft.fillRect(cx + S(29, k), cy - S(6, k), S(11, k), S(8, k), bg);
  // riga bassa con ricciolo verso il basso
  tft.fillRoundRect(cx - S(38, k), cy + S(22, k), S(60, k), S(6, k), S(3, k), c1);
  tft.fillCircle(cx + S(22, k), cy + S(33, k), S(10, k), c1);
  tft.fillCircle(cx + S(22, k), cy + S(33, k), S(5, k), bg);
}

int iconFor(int c) {
  if (c == 0 || c == 1) return IC_CLEAR;
  if (c == 2) return IC_PARTLY;
  if (c == 3) return IC_CLOUD;
  if (c == 45 || c == 48) return IC_FOG;
  if (c >= 51 && c <= 57) return IC_DRIZZLE;
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return IC_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return IC_SNOW;
  if (c >= 95) return IC_STORM;
  return IC_CLOUD;
}

const char* condText(int c) {
  switch (c) {
    case 0: return "Sereno";
    case 1: return "Quasi sereno";
    case 2: return "Poco nuvoloso";
    case 3: return "Coperto";
    case 45: case 48: return "Nebbia";
    case 51: case 53: case 55: return "Pioviggine";
    case 56: case 57: return "Pioviggine gelata";
    case 61: return "Pioggia debole";
    case 63: return "Pioggia";
    case 65: return "Pioggia forte";
    case 66: case 67: return "Pioggia gelata";
    case 71: return "Neve debole";
    case 73: return "Neve";
    case 75: return "Neve forte";
    case 77: return "Granelli di neve";
    case 80: return "Rovesci";
    case 81: case 82: return "Rovesci forti";
    case 85: case 86: return "Rovesci di neve";
    case 95: return "Temporale";
    case 96: case 99: return "Temp. grandine";
    default: return "Variabile";
  }
}

// ------------------------------------------------------------ Gesti ---------
enum Gesture { G_NONE, G_SINGLE, G_DOUBLE, G_LONG, G_VLONG };

static const uint32_t LONG_MS    = 800;
static const uint32_t VLONG_MS   = 8000;
static const uint32_t DOUBLE_MS  = 350;
static const uint32_t MIN_TAP_MS = 30;

bool inSplash = true;      // durante la schermata iniziale i gesti sono ignorati

float touchBase = 60;
uint16_t thrOn = 50, thrOff = 55;
uint16_t touchRaw = 0;
bool tState = false;
uint32_t stuckSince = 0;
uint32_t warmUntil = 0;    // fino a questo istante il touch si calibra e i gesti sono ignorati
uint8_t belowCnt = 0;

// Soglie relative alla base a riposo: il tocco abbassa il valore di almeno il 10 %
// (minimo 4 unita'), il rilascio avviene a meta' di quella distanza (isteresi).
static void setThr() {
  float drop = touchBase * 0.10f;
  if (drop < 4) drop = 4;
  thrOn = (uint16_t)(touchBase - drop);
  thrOff = (uint16_t)(touchBase - drop * 0.5f);
}

uint16_t readTouch() {          // mediana di 5 letture (il WiFi disturba il sensore)
  uint16_t v[5];
  for (int i = 0; i < 5; i++) v[i] = touchRead(PIN_TOUCH);
  for (int i = 1; i < 5; i++) {
    uint16_t k = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[2];
}

void calibrateTouch() {
  touchRead(PIN_TOUCH);          // la prima lettura puo' essere sporca
  delay(30);
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += readTouch();
    delay(8);
  }
  touchBase = sum / 16.0f;
  setThr();
  Serial.printf("Touch: base=%.0f soglia on=%u off=%u\n", touchBase, thrOn, thrOff);
}

bool touchPressed() {
  uint16_t v = readTouch();
  touchRaw = v;
  uint32_t now = millis();

  // Fase di assestamento (schermata iniziale e WiFi in avvio): la base segue il valore, nessun gesto.
  if (inSplash || (int32_t)(now - warmUntil) < 0) {
    touchBase += ((float)v - touchBase) / 8.0f;
    setThr();
    tState = false;
    belowCnt = 0;
    return false;
  }

  if (!tState) {
    if (v < thrOn) {
      if (++belowCnt >= 2) { tState = true; belowCnt = 0; }   // 2 letture di fila
    } else {
      belowCnt = 0;
    }
  } else if (v > thrOff) {
    tState = false;
    belowCnt = 0;
  }

  if (!tState) {
    stuckSince = 0;
    if (v >= thrOff) {                       // segue lentamente la deriva a riposo
      touchBase += ((float)v - touchBase) / 256.0f;
      setThr();
    }
  } else {
    if (!stuckSince) stuckSince = now;
    else if (now - stuckSince > 12000) {     // "premuto" da troppo: ricalibra
      touchBase = v;
      setThr();
      tState = false;
      stuckSince = 0;
    }
  }
  return tState;
}

Gesture readGesture() {
  static bool down = false, longF = false, vlongF = false;
  static uint32_t downAt = 0, lastRel = 0;
  static uint8_t taps = 0;
  uint32_t now = millis();
  bool p = touchPressed();

  if (p && !down) {
    down = true;
    downAt = now;
    longF = false;
    vlongF = false;
  } else if (p && down) {
    if (!longF && now - downAt >= LONG_MS) {
      longF = true;
      taps = 0;
      return G_LONG;
    }
    if (longF && !vlongF && now - downAt >= VLONG_MS) {
      vlongF = true;
      return G_VLONG;
    }
  } else if (!p && down) {
    down = false;
    if (!longF && now - downAt >= MIN_TAP_MS) {
      taps++;
      lastRel = now;
      if (taps >= 2) {
        taps = 0;
        return G_DOUBLE;
      }
    }
  }
  if (!down && taps == 1 && now - lastRel >= DOUBLE_MS) {
    taps = 0;
    return G_SINGLE;
  }
  return G_NONE;
}

// ------------------------------------------------------ Stato schermate -----
static const int NMENU = 2;          // 0 = Meteo, 1 = Aria
int menuIdx = 0;
int funcIdx = 0;
bool inFunc = false;
bool needFull = true;

int funcCount(int m) { return m == 0 ? 3 : 2; }

String cityName() { return (cfg.geo && cfg.city.length()) ? cfg.city : cfg.cityq; }

const char* menuName(int m) { return m == 0 ? "Meteo" : "Aria"; }

const char* funcName(int m, int f) {
  if (m == 0) return f == 0 ? "Principale" : (f == 1 ? "Prossimi 3 giorni" : "Grafico temperatura");
  return f == 0 ? "Qualità dell'aria" : "Salute";
}

String stateText() {
  if (inSplash) return String("Avvio in corso...");
  if (!inFunc) return String("Menu: ") + menuName(menuIdx);
  return String(menuName(menuIdx)) + " - " + funcName(menuIdx, funcIdx);
}

// ------------------------------------------------------------- MENU ---------
void menuFull() {
  tft.fillScreen(C_BG);
  const char* title = menuName(menuIdx);

  // titolo grande con linea neon
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(title, 120, 12, 4);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.fillRoundRect(70, 70, 100, 3, 1, C_ACC2);
  tft.fillRoundRect(95, 76, 50, 2, 1, C_ACC);

  // anelli decorativi dietro la grafica
  tft.drawCircle(120, 150, 68, c565(mixRGB(th->bg, th->a1, 110)));
  tft.drawCircle(120, 150, 70, c565(mixRGB(th->bg, th->a2, 80)));

  if (menuIdx == 0) drawIcon(120, 150, IC_PARTLY, true, 1.6f, C_BG);
  else iconWind(120, 146, 1.15f, C_AIR, RGB565(150, 235, 255), C_BG);

  // pallini di scorrimento
  for (int i = 0; i < NMENU; i++) {
    int x = 120 + (int)lroundf((i - (NMENU - 1) / 2.0f) * 20);
    tft.fillCircle(x, 229, 5, i == menuIdx ? C_ACC : C_TRACK);
  }
}

// -------------------------------------------------------- Schermata iniziale -
uint32_t splashStart = 0;
String splashIp = "";
bool splashIpDrawn = false;

void splashStatic() {
  tft.fillScreen(C_BG);
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString("Small TV", 120, 14, 4);
  tft.setTextSize(1);
  tft.setTextColor(C_ACC2, C_BG);
  tft.drawString("Evolution Pro", 120, 76, 4);
  tft.setTextDatum(TL_DATUM);
  tft.fillRoundRect(40, 116, 160, 3, 1, C_ACC);
  tft.fillRoundRect(70, 122, 100, 2, 1, C_ACC2);
  drawStr("Non toccare il pulsante", 120, 192, 2, 1, C_DIM, C_BG);
  tft.fillRoundRect(20, 216, 200, 6, 3, C_TRACK);
  splashIpDrawn = false;
}

void splashTick() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  if (now - lastDraw < 100) return;
  lastDraw = now;
  uint32_t el = now - splashStart;

  bool con = (WiFi.status() == WL_CONNECTED);
  String ip = con ? WiFi.localIP().toString() : String("");
  if (!splashIpDrawn || ip != splashIp) {
    splashIp = ip;
    splashIpDrawn = true;
    tft.fillRect(0, 134, 240, 54, C_BG);
    if (con) {
      drawStr("Indirizzo IP", 120, 136, 2, 1, C_DIM, C_BG);
      drawStr(ip, 120, 156, 4, 1, C_TEXT, C_BG);
    } else {
      drawStr("Connessione WiFi...", 120, 150, 2, 1, C_DIM, C_BG);
    }
  }

  uint32_t e = el > 6000 ? 6000 : el;
  int w = (int)(e * 200UL / 6000UL);
  if (w > 0) tft.fillRoundRect(20, 216, w < 6 ? 6 : w, 6, 3, C_ACC);

  if (el >= 6000 && (con || el >= 12000)) {
    inSplash = false;
    warmUntil = now + 1500;      // ancora un attimo di assestamento dopo la connessione
    needFull = true;
  }
}

// ------------------------------------------------------------ METEO 1 -------
static int lastSec = -1;
static uint32_t rotIdx = 0xFFFFFFFF;

void drawDate(const struct tm* t) {
  tft.fillRect(0, 4, 240, 18, C_BG);
  if (!t) {
    drawStr("Sincronizzo l'ora...", 120, 4, 2, 1, C_DIM, C_BG);
    return;
  }
  char b[48];
  snprintf(b, sizeof b, "%s %d %s %d", GIORNI[t->tm_wday], t->tm_mday, MESI[t->tm_mon], t->tm_year + 1900);
  drawStr(b, 120, 4, 2, 1, C_ACC2, C_BG);
}

void drawClock(const struct tm* t) {
  String s;
  bool pm = false;
  uint16_t col = C_TEXT;
  if (!t) {
    s = "00:00";
    col = C_TRACK;
  } else {
    int h = t->tm_hour;
    char b[16];
    if (cfg.fmt12) {
      pm = h >= 12;
      h %= 12;
      if (h == 0) h = 12;
      snprintf(b, sizeof b, "%d:%02d", h, t->tm_min);
    } else {
      snprintf(b, sizeof b, "%02d:%02d", h, t->tm_min);
    }
    s = b;
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col, C_BG);
  tft.setTextPadding(176);
  tft.drawString(s, 120, 24, 7);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);

  int w = tft.textWidth(s, 7);
  int xr = 120 + w / 2 + 3;
  if (xr < 240) tft.fillRect(xr, 46, 240 - xr, 30, C_BG);
  if (t && cfg.fmt12) drawStr(pm ? "PM" : "AM", xr + 2, 50, 4, 0, C_ACC, C_BG);
}

void drawSecBar(const struct tm* t) {
  int w = 0;
  if (t) w = (t->tm_sec + 1) * 220 / 60;
  if (w > 0) tft.fillRect(10, 78, w, 6, C_ACC);
  if (w < 220) tft.fillRect(10 + w, 78, 220 - w, 6, C_TRACK);
}

// tre riquadri centrati (Min, Max, Umidita') con caratteri grandi
void statCards(int y, const char* l0, const String& v0, uint16_t c0,
               const char* l1, const String& v1, uint16_t c1,
               const char* l2, const String& v2, uint16_t c2, int h) {
  const int cx[3] = {44, 120, 196};
  const char* lab[3] = {l0, l1, l2};
  const String* val[3] = {&v0, &v1, &v2};
  const uint16_t cc[3] = {c0, c1, c2};
  const uint16_t edge = c565(mixRGB(th->card, th->a1, 110));
  for (int i = 0; i < 3; i++) {
    int x = cx[i] - 36;
    tft.fillRoundRect(x, y, 72, h, 10, C_CARD);
    tft.drawRoundRect(x, y, 72, h, 10, edge);
    drawStr(lab[i], cx[i], y + 3, 2, 1, C_DIM, C_CARD);
    drawStr(*val[i], cx[i], y + (h - 26) / 2 + 8, 4, 1, cc[i], C_CARD);
  }
}

void drawWeatherBlock() {
  tft.fillRect(0, 90, 240, 102, C_BG);
  if (!W.valid) {
    drawIcon(46, 116, IC_CLOUD, true, 0.95f, C_BG);
    drawStr("--", 100, 106, 4, 0, C_DIM, C_BG);
    statCards(145, "MIN", "--", C_DIM, "MAX", "--", C_DIM, "UMID.", "--", C_DIM, 46);
    return;
  }
  drawIcon(46, 116, iconFor(W.code), W.day, 0.95f, C_BG);

  int tv = (int)lroundf(W.t);
  bool neg = tv < 0;
  String ts = String(abs(tv));
  int tw = tft.textWidth(ts, 6);
  int total = tw + (neg ? 20 : 0);
  int x0 = 94;
  if (x0 + total + 34 > 238) x0 = 238 - 34 - total;
  int y0 = 92;
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(TL_DATUM);
  if (neg) tft.fillRect(x0, y0 + 34, 14, 7, C_TEXT);
  tft.drawString(ts, x0 + (neg ? 20 : 0), y0, 6);
  int xe = x0 + total + 4;
  tft.drawCircle(xe + 5, y0 + 8, 4, C_TEXT);
  tft.drawCircle(xe + 5, y0 + 8, 3, C_TEXT);
  drawStr("C", xe + 14, y0 + 2, 4, 0, C_TEXT, C_BG);

  statCards(145, "MIN", String((int)lroundf(W.tmin[0])) + "~", C_MIN,
            "MAX", String((int)lroundf(W.tmax[0])) + "~", C_MAX,
            "UMID.", String((int)lroundf(W.hum)) + "%", C_ACC, 46);
}

void drawCondLine() {
  tft.fillRoundRect(8, 194, 224, 30, 8, C_CARD);
  String s;
  uint16_t col = C_TEXT;
  if (!W.valid) {
    s = statusText();
    if (!s.length()) s = "In attesa dei dati";
    col = C_DIM;
  } else {
    switch (rotIdx % 6) {
      case 0: s = condText(W.code); col = C_TEXT; break;
      case 1: s = "Percepita " + String((int)lroundf(W.feels)) + "~"; col = C_MAX; break;
      case 2: s = "Vento " + String((int)lroundf(W.wind)) + " km/h"; col = C_ACC; break;
      case 3: s = "Atm " + String((int)lroundf(W.press)) + " hPa"; col = C_CLOUD; break;
      case 4: s = "Alba " + fmtHM(W.srH, W.srM); col = C_SUN; break;
      default: s = "Tramonto " + fmtHM(W.ssH, W.ssM); col = RGB565(255, 140, 100); break;
    }
  }
  uint8_t f = 4;
  if (strW(s, 4) > 212) f = 2;
  drawStr(s, 120, f == 4 ? 196 : 201, f, 1, col, C_CARD);
}

void drawFooter() {
  tft.fillRect(0, 224, 240, 16, C_BG);
  drawStr(cityName(), 8, 224, 2, 0, C_DIM, C_BG);
  if (gStatus != ST_OK || !W.valid) {
    String s = statusText();
    if (s.length()) drawStr(s, 232, 224, 2, 2, (gStatus == ST_ERR_GEO || gStatus == ST_ERR_NET) ? C_ERR : C_DIM, C_BG);
  } else if (W.upd) {
    struct tm u;
    localtime_r(&W.upd, &u);
    drawStr("agg. " + fmtHM(u.tm_hour, u.tm_min), 232, 224, 2, 2, C_DIM, C_BG);
  }
}

void meteoFull() {
  tft.fillScreen(C_BG);
  struct tm t;
  bool ok = getTm(t);
  const struct tm* tp = ok ? &t : nullptr;
  lastSec = ok ? t.tm_sec : -1;
  rotIdx = (millis() / 4000) % 6;
  drawDate(tp);
  drawClock(tp);
  drawSecBar(tp);
  drawWeatherBlock();
  drawCondLine();
  drawFooter();
}

void meteoTick(bool dataChanged) {
  struct tm t;
  bool ok = getTm(t);
  if (ok && t.tm_sec != lastSec) {
    static int lastDay = -1;
    if (lastSec == -1 || t.tm_mday != lastDay) {   // primo orario valido o cambio giorno
      drawDate(&t);
      lastDay = t.tm_mday;
    }
    lastSec = t.tm_sec;
    drawClock(&t);
    drawSecBar(&t);
  }
  uint32_t r = (millis() / 4000) % 6;
  if (r != rotIdx) {
    rotIdx = r;
    drawCondLine();
  }
  if (dataChanged) {
    drawWeatherBlock();
    drawCondLine();
    drawFooter();
  }
}

// ------------------------------------------------------------ METEO 2 -------
void previsioniFull() {
  tft.fillScreen(C_BG);
  drawStr("Prossimi 3 giorni", 120, 3, 2, 1, C_ACC2, C_BG);
  if (!W.valid) {
    String s = statusText();
    if (!s.length()) s = "In attesa dei dati";
    drawStr(s, 120, 110, 4, 1, C_DIM, C_BG);
    return;
  }
  for (int i = 1; i <= 3; i++) {
    int y = 22 + (i - 1) * 72;
    tft.fillRoundRect(6, y, 228, 66, 10, C_CARD);
    drawIcon(38, y + 33, iconFor(W.dcode[i]), true, 0.62f, C_CARD);

    String label;
    if (i == 1) {
      label = "Domani";
    } else {
      int yy = W.dday[i].substring(0, 4).toInt();
      int mm = W.dday[i].substring(5, 7).toInt();
      int dd = W.dday[i].substring(8, 10).toInt();
      if (mm >= 1 && mm <= 12) label = String(GIORNI[dowOf(yy, mm, dd)]) + " " + String(dd);
      else label = "--";
    }
    drawStr(label, 72, y + 6, 4, 0, C_TEXT, C_CARD);
    drawStr(condText(W.dcode[i]), 72, y + 40, 2, 0, C_DIM, C_CARD);
    drawStr(String((int)lroundf(W.tmax[i])) + "~", 228, y + 6, 4, 2, C_MAX, C_CARD);
    drawStr(String((int)lroundf(W.tmin[i])) + "~", 228, y + 36, 4, 2, C_MIN, C_CARD);
  }
}

// ------------------------------------------------------------ METEO 3 -------
// Grafico della temperatura nelle prossime 24 ore
void graficoFull() {
  tft.fillScreen(C_BG);
  drawStr("Temperatura - 24 ore", 120, 3, 2, 1, C_ACC2, C_BG);
  if (!W.valid || !W.hvalid) {
    String s = statusText();
    if (!s.length()) s = "In attesa dei dati";
    drawStr(s, 120, 110, 4, 1, C_DIM, C_BG);
    return;
  }

  float mn = W.hh[0], mx = W.hh[0];
  int imn = 0, imx = 0;
  for (int i = 1; i < 24; i++) {
    if (W.hh[i] < mn) { mn = W.hh[i]; imn = i; }
    if (W.hh[i] > mx) { mx = W.hh[i]; imx = i; }
  }
  float lo = floorf(mn) - 1.0f, hi = ceilf(mx) + 1.0f;
  if (hi - lo < 4.0f) {
    float m = (hi + lo) / 2.0f;
    lo = m - 2.0f;
    hi = m + 2.0f;
  }

  const int X0 = 18, X1 = 222, Y0 = 42, Y1 = 140;
  const int NX = X1 - X0 + 1;
  static int16_t ys[205];

  tft.fillRoundRect(6, 22, 228, 148, 12, C_CARD);
  tft.drawRoundRect(6, 22, 228, 148, 12, c565(mixRGB(th->card, th->a1, 110)));

  // area sotto la curva
  const uint16_t areaCol = c565(mixRGB(th->card, th->a1, 70));
  for (int x = X0; x <= X1; x++) {
    float f = (float)(x - X0) / (float)(X1 - X0) * 23.0f;
    int i = (int)f;
    if (i > 22) i = 22;
    float fr = f - (float)i;
    float v = W.hh[i] + (W.hh[i + 1] - W.hh[i]) * fr;
    int y = Y1 - (int)lroundf((v - lo) / (hi - lo) * (float)(Y1 - Y0));
    ys[x - X0] = (int16_t)y;
    tft.drawFastVLine(x, y, Y1 - y + 1, areaCol);
  }

  // griglia tratteggiata
  const uint16_t gridCol = c565(mixRGB(th->card, th->dim, 120));
  for (int g = 0; g <= 3; g++) {
    int y = Y0 + (Y1 - Y0) * g / 3;
    for (int x = X0; x <= X1; x += 8) tft.drawFastHLine(x, y, 3, gridCol);
  }

  // curva (spessore 2)
  for (int x = 1; x < NX; x++) {
    tft.drawLine(X0 + x - 1, ys[x - 1], X0 + x, ys[x], C_ACC);
    tft.drawLine(X0 + x - 1, ys[x - 1] + 1, X0 + x, ys[x] + 1, C_ACC);
  }

  // ora attuale
  tft.fillCircle(X0, ys[0], 5, C_ACC2);
  tft.drawCircle(X0, ys[0], 5, C_TEXT);

  // minimo e massimo con il valore
  int xmx = X0 + (X1 - X0) * imx / 23, xmn = X0 + (X1 - X0) * imn / 23;
  int ymx = ys[xmx - X0], ymn = ys[xmn - X0];
  tft.fillCircle(xmx, ymx, 3, C_MAX);
  tft.fillCircle(xmn, ymn, 3, C_MIN);
  int lx = xmx < 30 ? 30 : (xmx > 210 ? 210 : xmx);
  drawStr(String((int)lroundf(mx)) + "~", lx, ymx - 20, 2, 1, C_MAX, C_CARD);
  lx = xmn < 30 ? 30 : (xmn > 210 ? 210 : xmn);
  drawStr(String((int)lroundf(mn)) + "~", lx, ymn - 20, 2, 1, C_MIN, C_CARD);

  // etichette delle ore
  const int idx[4] = {0, 6, 12, 18};
  for (int k = 0; k < 4; k++) {
    int x = X0 + (X1 - X0) * idx[k] / 23;
    int xl = x < 30 ? 30 : x;
    drawStr(hourLabel((W.hstart + idx[k]) % 24), xl, 148, 2, 1, C_DIM, C_CARD);
  }

  statCards(176, "ADESSO", String((int)lroundf(W.t)) + "~", C_TEXT,
            "MIN", String((int)lroundf(mn)) + "~", C_MIN,
            "MAX", String((int)lroundf(mx)) + "~", C_MAX, 56);
}

// ------------------------------------------------------------- ARIA ---------
void ringSeg(int cx, int cy, int r0, int r1, float a0, float a1, uint16_t col) {
  const float step = 3.0f, D = 0.0174533f;
  for (float a = a0; a > a1 + 0.01f; a -= step) {
    float b = a - step;
    if (b < a1) b = a1;
    float ca = cosf(a * D), sa = sinf(a * D), cb = cosf(b * D), sb = sinf(b * D);
    int x0 = cx + (int)lroundf(r0 * ca), y0 = cy - (int)lroundf(r0 * sa);
    int x1 = cx + (int)lroundf(r1 * ca), y1 = cy - (int)lroundf(r1 * sa);
    int x2 = cx + (int)lroundf(r1 * cb), y2 = cy - (int)lroundf(r1 * sb);
    int x3 = cx + (int)lroundf(r0 * cb), y3 = cy - (int)lroundf(r0 * sb);
    tft.fillTriangle(x0, y0, x1, y1, x2, y2, col);
    tft.fillTriangle(x0, y0, x2, y2, x3, y3, col);
  }
}

int aqiBand(float v) {
  if (v < 20) return 0;
  if (v < 40) return 1;
  if (v < 60) return 2;
  if (v < 80) return 3;
  if (v < 100) return 4;
  return 5;
}

void pmRow(int y, const char* label, float v, float scale, const float* thr) {
  int band = 0;
  while (band < 5 && v >= thr[band]) band++;
  drawStr(label, 10, y, 2, 0, C_TEXT, C_BG);
  char b[24];
  snprintf(b, sizeof b, "%.1f ug/m3", v);
  drawStr(b, 230, y, 2, 2, BANDCOL[band], C_BG);
  tft.fillRoundRect(10, y + 19, 220, 7, 3, C_TRACK);
  float f = v / scale;
  if (f > 1) f = 1;
  if (f < 0) f = 0;
  int w = (int)lroundf(220 * f);
  if (w < 6 && v > 0) w = 6;
  if (w > 0) tft.fillRoundRect(10, y + 19, w, 7, 3, BANDCOL[band]);
}

void airFull() {
  tft.fillScreen(C_BG);
  drawStr("EAQI", 8, 2, 2, 0, C_DIM, C_BG);
  const int cx = 120, cy = 116;
  for (int i = 0; i < 6; i++) ringSeg(cx, cy, 64, 96, 179.0f - 30 * i, 151.0f - 30 * i, BANDCOL[i]);

  if (!A.valid) {
    String s = statusText();
    if (!s.length()) s = "In attesa dei dati";
    drawStr(s, 120, 84, 4, 1, C_DIM, C_BG);
  } else {
    float v = A.aqi;
    if (v < 0) v = 0;
    if (v > 120) v = 120;
    float ang = (180.0f - 180.0f * v / 120.0f) * 0.0174533f;
    float ca = cosf(ang), sa = sinf(ang);
    int tx = cx + (int)lroundf(100 * ca), ty = cy - (int)lroundf(100 * sa);
    float bx = cx + 114 * ca, by = cy - 114 * sa;
    tft.fillTriangle(tx, ty,
                     (int)lroundf(bx + 7 * sa), (int)lroundf(by + 7 * ca),
                     (int)lroundf(bx - 7 * sa), (int)lroundf(by - 7 * ca), C_TEXT);

    int band = aqiBand(A.aqi);
    int val = (int)lroundf(A.aqi);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(String(val), 120, val >= 100 ? 76 : 66, 7);
    tft.setTextDatum(TL_DATUM);
    drawStr(BANDTXT[band], 120, 126, 4, 1, BANDCOL[band], C_BG);

    static const float thPm25[5] = {10, 20, 25, 50, 75};
    static const float thPm10[5] = {20, 40, 50, 100, 150};
    pmRow(160, "PM2.5", A.pm25, 100, thPm25);
    pmRow(192, "PM10", A.pm10, 200, thPm10);
  }

  tft.fillRect(0, 224, 240, 16, C_BG);
  drawStr(cityName(), 8, 224, 2, 0, C_DIM, C_BG);
  if (gStatus != ST_OK || !A.valid) {
    String s = statusText();
    if (s.length()) drawStr(s, 232, 224, 2, 2, (gStatus == ST_ERR_GEO || gStatus == ST_ERR_NET) ? C_ERR : C_DIM, C_BG);
  } else if (A.upd) {
    struct tm u;
    localtime_r(&A.upd, &u);
    drawStr("agg. " + fmtHM(u.tm_hour, u.tm_min), 232, 224, 2, 2, C_DIM, C_BG);
  }
}

// ------------------------------------------------------------ ARIA 2 --------
// Salute: indice UV e pollini (graminacee, olivo, betulla), come in SmallTV Evolution
static const uint32_t UV_COL[5] = {0x3DDC84, 0xF2D338, 0xFF8A3D, 0xE5384F, 0xA24BD6};
static const char* const UV_NAME[5] = {"Basso", "Moderato", "Alto", "Molto alto", "Estremo"};
static const char* const UV_TIP[5] = {"Nessun rischio", "Metti la crema", "Crema e ombra", "Evita il sole", "Resta all'ombra"};
static const uint32_t PO_COL[5] = {0x6B7399, 0x3DDC84, 0xF2D338, 0xFF8A3D, 0xE5384F};
static const char* const PO_NAME[5] = {"Assenti", "Bassi", "Moderati", "Alti", "Molto alti"};

// livello UV: 0 basso (0-2), 1 moderato (3-5), 2 alto (6-7), 3 molto alto (8-10), 4 estremo (11+)
uint8_t uvLevel(float uv) { return uv < 3.0f ? 0 : uv < 6.0f ? 1 : uv < 8.0f ? 2 : uv < 11.0f ? 3 : 4; }

// livello pollini (granuli/m3): 0 assenti, 1 bassi, 2 moderati, 3 alti, 4 molto alti.
// Soglie indicative (tipo: 0 graminacee, 1 olivo, 2 betulla).
uint8_t pollenLevel(float v, uint8_t kind) {
  static const float T[3][3] = {{19, 50, 150}, {19, 50, 150}, {10, 70, 300}};  // fine "bassi", "moderati", "alti"
  if (v < 1.0f) return 0;
  const float* t = T[kind > 2 ? 2 : kind];
  return v < t[0] ? 1 : v < t[1] ? 2 : v < t[2] ? 3 : 4;
}

void healthFull() {
  const int oy = -8;
  const uint16_t cbg = C_CARD;
  tft.fillScreen(C_BG);
  drawStr("Salute", 120, 3, 2, 1, C_ACC2, C_BG);
  if (!A.valid) {
    String s = statusText();
    if (!s.length()) s = "In attesa dei dati";
    drawStr(s, 120, 110, 4, 1, C_DIM, C_BG);
    return;
  }
  tft.fillRoundRect(6, 25, 228, 209, 14, cbg);
  tft.drawRoundRect(6, 25, 228, 209, 14, c565(mixRGB(th->card, th->a1, 100)));
  const uint32_t inner = mixRGB(th->card, th->bg, 110);

  // --- indice UV: numero grande a sinistra, livello e consiglio a destra, scala a 5 fasce con indicatore
  const uint8_t ul = uvLevel(A.uv);
  const uint32_t uc = UV_COL[ul];
  char b[24];
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, cbg);
  tft.drawString("INDICE UV", 16, 40 + oy, 2);
  if (A.uvMax > 0.05f) {   // massimo previsto oggi (di notte l'indice attuale e' 0)
    char m[20];
    snprintf(m, sizeof(m), "max oggi %.1f", A.uvMax);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(m, 224, 40 + oy, 2);
    tft.setTextDatum(TL_DATUM);
  }
  tft.setTextDatum(MC_DATUM);
  snprintf(b, sizeof(b), A.uv < 10.0f ? "%.1f" : "%.0f", A.uv);
  tft.setTextColor(c565(uc), cbg);
  tft.drawString(b, 62, 80 + oy, 6);
  const char* nm = UV_NAME[ul];
  tft.setTextColor(c565(uc), cbg);
  tft.drawString(nm, 172, 68 + oy, tft.textWidth(nm, 4) <= 108 ? 4 : 2);
  tft.setTextColor(c565(mixRGB(th->dim, th->text, 110)), cbg);
  tft.drawString(UV_TIP[ul], 172, 94 + oy, 2);
  // scala 0..12: 5 fasce (0-3, 3-6, 6-8, 8-11, 11-12) con la fascia attuale accesa
  static const uint8_t EDGE[6] = {0, 3, 6, 8, 11, 12};
  const int sx = 16, sw = 208, sy = 112 + oy, sh = 8;
  for (int i = 0; i < 5; i++) {
    const int x0 = sx + sw * EDGE[i] / 12, x1 = sx + sw * EDGE[i + 1] / 12;
    tft.fillRoundRect(x0 + 1, sy, x1 - x0 - 2, sh, 3, c565(i == ul ? UV_COL[i] : mixRGB(th->card, UV_COL[i], 70)));
  }
  float up = A.uv > 12.0f ? 12.0f : A.uv;
  const int mx = sx + (int)(sw * up / 12.0f);
  tft.fillTriangle(mx - 5, sy + sh + 2, mx + 5, sy + sh + 2, mx, sy + sh - 3, 0xFFFF);

  tft.drawFastHLine(16, 134 + oy, 208, c565(mixRGB(th->card, th->dim, 70)));

  // --- pollini
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, cbg);
  tft.drawString("POLLINI", 16, 137 + oy, 2);
  if (A.hasPollen && pollenLevel(A.pGrass, 0) == 0 && pollenLevel(A.pOlive, 1) == 0 && pollenLevel(A.pBirch, 2) == 0) {
    tft.setTextDatum(TR_DATUM);
    tft.drawString("nessuno rilevato", 224, 137 + oy, 2);
    tft.setTextDatum(TL_DATUM);
  }
  if (!A.hasPollen) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(c565(mixRGB(th->dim, th->text, 110)), cbg);
    tft.drawString("Non disponibili", 120, 182, 4);
    tft.setTextColor(C_DIM, cbg);
    tft.drawString("fuori stagione o zona", 120, 208, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  static const char* const LAB[3] = {"Graminacee", "Olivo", "Betulla"};
  const float val[3] = {A.pGrass, A.pOlive, A.pBirch};
  for (int i = 0; i < 3; i++) {
    const int y = 149 + i * 28, h = 24;
    const uint8_t lv = pollenLevel(val[i], (uint8_t)i);
    const uint32_t pc = PO_COL[lv];
    const uint16_t ib = c565(inner);
    tft.fillRoundRect(12, y, 216, h, 8, ib);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_TEXT, ib);
    tft.drawString(LAB[i], 20, y + h / 2 + 1, 2);
    // 4 tacche: tante accese quanto il livello
    for (int k = 0; k < 4; k++) tft.fillRoundRect(98 + k * 14, y + 7, 11, 10, 3, c565(k < lv ? pc : mixRGB(th->card, th->dim, 60)));
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(c565(pc), ib);
    tft.drawString(PO_NAME[lv], 222, y + h / 2 + 1, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

// ------------------------------------------------------ Interfaccia web -----
void handleGesture(Gesture g);   // definita piu' sotto

bool otaOk = false, otaBegun = false, otaFirst = false;
String otaErr = "";

void otaScreen(const char* l1, const char* l2) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  drawStr("Aggiornamento", 120, 60, 4, 1, C_ACC, C_BG);
  drawStr(l1, 120, 110, 4, 1, C_TEXT, C_BG);
  drawStr(l2, 120, 150, 2, 1, C_DIM, C_BG);
}

String buildPage(bool ap) {
  String h;
  h.reserve(9000);
  h += R"HTML(<!DOCTYPE html><html lang='it'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>Small TV Evolution Pro</title><style>
:root{--bg:#05060f;--card:#0e1230;--a1:#00e5ff;--a2:#ff2e93;--dim:#8b95bd}
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;background:var(--bg);color:#fff;margin:0 auto;padding:16px;max-width:480px}
h1{font-size:22px;margin:4px 0 14px;color:var(--a1)}h1 span{color:var(--a2)}
label{display:block;margin:14px 0 4px;color:var(--dim);font-size:14px}
input,select{width:100%;padding:12px;border-radius:10px;border:1px solid #2a3470;background:#0b1030;color:#fff;font-size:16px}
button.main{margin-top:22px;width:100%;padding:14px;border:0;border-radius:10px;background:var(--a1);color:#001018;font-size:17px;font-weight:700}
.c{display:flex;align-items:center;gap:10px;margin-top:14px}.c input{width:auto}
small{color:#7d8db3;display:block;margin-top:6px}
.nav{position:sticky;top:0;z-index:5;background:var(--bg);padding:8px 0 10px;margin-bottom:6px}
.keys{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
.keys button{height:58px;border-radius:14px;border:1.5px solid var(--a1);background:var(--card);color:var(--a1);display:flex;align-items:center;justify-content:center}
.keys button:nth-child(2){border-color:var(--a2);color:var(--a2)}
.keys button:active{background:#18204a}
.st{text-align:center;color:var(--dim);font-size:13px;margin-top:8px;min-height:18px}
.card{background:var(--card);border:1px solid #2a3470;border-radius:12px;padding:14px;margin-top:22px}
.card h2{margin:0 0 6px;font-size:17px;color:var(--a1)}
progress{width:100%;height:12px;margin-top:12px}
</style></head><body><h1>Small TV <span>Evolution Pro</span></h1>)HTML";

  if (!ap) {
    h += R"HTML(<div class='nav'><div class='keys'>
<button type='button' onclick="k('prev')" aria-label='Indietro (doppio click)'><svg viewBox='0 0 24 24' width='30' height='30'><path d='M15 5l-7 7 7 7' fill='none' stroke='currentColor' stroke-width='2.6' stroke-linecap='round' stroke-linejoin='round'/></svg></button>
<button type='button' onclick="k('enter')" aria-label='Entra / Esci (pressione lunga)'><svg viewBox='0 0 24 24' width='30' height='30'><path d='M10 4H5v16h5M13 8l4 4-4 4M8 12h9' fill='none' stroke='currentColor' stroke-width='2.4' stroke-linecap='round' stroke-linejoin='round'/></svg></button>
<button type='button' onclick="k('next')" aria-label='Avanti (click)'><svg viewBox='0 0 24 24' width='30' height='30'><path d='M9 5l7 7-7 7' fill='none' stroke='currentColor' stroke-width='2.6' stroke-linecap='round' stroke-linejoin='round'/></svg></button>
</div><div class='st' id='st'>&nbsp;</div></div>)HTML";
  }

  h += "<form method='POST' action='/save'>";
  h += "<label>Rete WiFi</label>";
  if (ap && netOpts.length()) {
    h += "<select name='ssid'><option value=''>-- scegli una rete --</option>";
    h += netOpts;
    h += "</select><label>oppure scrivi il nome della rete</label>";
  }
  h += "<input name='ssid_manual' autocapitalize='off' placeholder='" + htmlEsc(cfg.ssid) + "'>";
  h += "<label>Password WiFi</label><input name='pass' type='password' placeholder='lascia vuoto per non cambiarla'>";
  h += "<label>Citt&agrave;</label><input name='cityq' value='" + htmlEsc(cfg.cityq) + "'>";
  h += "<small>Scrivi il nome (es. Roma, Chieti, Milano). Le coordinate si trovano da sole.</small>";
  h += "<label>Formato ora</label><select name='fmt'><option value='24'";
  if (!cfg.fmt12) h += " selected";
  h += ">24 ore</option><option value='12'";
  if (cfg.fmt12) h += " selected";
  h += ">12 ore (AM/PM)</option></select>";
  h += "<label>Tema colori</label><select name='theme'>";
  for (int i = 0; i < THEME_COUNT; i++) {
    h += "<option value='" + String(i) + "'";
    if (i == cfg.theme) h += " selected";
    h += ">" + String(THEME_NAME[i]) + "</option>";
  }
  h += "</select>";
  h += "<div class='c'><input type='checkbox' name='inv' id='inv'";
  if (cfg.inv) h += " checked";
  h += "><label for='inv' style='margin:0'>Inverti i colori (se lo schermo sembra un negativo)</label></div>";
  h += "<div class='c'><input type='checkbox' name='rot' id='rot'";
  if (cfg.rot) h += " checked";
  h += "><label for='rot' style='margin:0'>Ruota lo schermo di 180&deg;</label></div>";
  h += "<button class='main' type='submit'>Salva e riavvia</button></form>";

  if (!ap) {
    h += R"HTML(<div class='card'><h2>Aggiornamento firmware (OTA)</h2>
<small style='margin-top:0'>Scegli il file <b>nmtv-firmware-app.bin</b> (non quello &laquo;completo&raquo;) e premi Aggiorna. Il dispositivo si riavvia da solo.</small>
<input type='file' id='fw' accept='.bin' style='margin-top:12px'>
<button class='main' type='button' onclick='ota()'>Aggiorna</button>
<progress id='pg' value='0' max='100'></progress><div class='st' id='om'>&nbsp;</div></div>
<script>
function $(i){return document.getElementById(i)}
function k(b){fetch('/tasto?b='+b).then(function(r){return r.text()}).then(function(t){$('st').textContent=t}).catch(function(){})}
function s(){fetch('/stato').then(function(r){return r.text()}).then(function(t){$('st').textContent=t}).catch(function(){})}
setInterval(s,3000);s();
function ota(){var f=$('fw').files[0];if(!f){alert('Scegli prima il file .bin');return}
var fd=new FormData();fd.append('firmware',f,f.name);var x=new XMLHttpRequest();x.open('POST','/update');
x.upload.onprogress=function(e){if(e.lengthComputable){$('pg').value=e.loaded/e.total*100}};
x.onload=function(){$('om').textContent=(x.responseText=='OK')?'Aggiornamento completato: il dispositivo si riavvia...':'Errore: '+x.responseText};
x.onerror=function(){$('om').textContent='Connessione interrotta (se la barra era al 100% il dispositivo si sta riavviando)'};
$('om').textContent='Caricamento in corso, non scollegare il dispositivo...';x.send(fd)}
</script>)HTML";
  }
  h += "<small style='margin-top:18px'>Firmware ";
  h += FW_VERSION;
  h += "</small></body></html>";
  return h;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", buildPage(WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA));
}

void handleSave() {
  String ssid = server.arg("ssid_manual");
  ssid.trim();
  if (!ssid.length()) {
    ssid = server.arg("ssid");
    ssid.trim();
  }
  String pass = server.arg("pass");
  if (ssid.length() && ssid != cfg.ssid) {
    cfg.ssid = ssid;
    cfg.pass = pass;
  } else if (pass.length()) {
    cfg.pass = pass;
  }
  String c = server.arg("cityq");
  c.trim();
  if (c.length() && c != cfg.cityq) {
    cfg.cityq = c;
    cfg.geo = false;
  }
  cfg.fmt12 = server.arg("fmt") == "12";
  int t = server.arg("theme").toInt();
  cfg.theme = (t >= 0 && t < THEME_COUNT) ? (uint8_t)t : 0;
  cfg.inv = server.hasArg("inv");
  cfg.rot = server.hasArg("rot");
  saveCfg();
  server.send(200, "text/html; charset=utf-8",
              String("<!DOCTYPE html><html lang='it'><head><meta charset='utf-8'>"
                     "<meta name='viewport' content='width=device-width,initial-scale=1'></head>"
                     "<body style='font-family:system-ui;background:#05060f;color:#e8eefc;padding:30px'>"
                     "<h2>Salvato!</h2><p>Il dispositivo si riavvia...</p></body></html>"));
  delay(1500);
  ESP.restart();
}

// i tre tasti della pagina web: indietro = doppio click, entra/esci = pressione lunga, avanti = click
void handleKey() {
  String b = server.arg("b");
  if (!inSplash && !gPortal) {
    if (b == "prev") handleGesture(G_DOUBLE);
    else if (b == "enter") handleGesture(G_LONG);
    else if (b == "next") handleGesture(G_SINGLE);
  }
  server.send(200, "text/plain; charset=utf-8", stateText());
}

void handleState() {
  server.send(200, "text/plain; charset=utf-8", stateText());
}

// aggiornamento firmware da browser
void handleUpdateUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    otaOk = true;
    otaBegun = false;
    otaFirst = true;
    otaErr = "";
    gPortal = true;                 // ferma il task dati durante l'aggiornamento
    if (u.filename.indexOf("completo") >= 0) {
      otaOk = false;
      otaErr = "usa il file nmtv-firmware-app.bin, non quello completo";
    } else {
      otaScreen("Caricamento...", "Non scollegare");
      Serial.printf("OTA: %s\n", u.filename.c_str());
      if (Update.begin(UPDATE_SIZE_UNKNOWN)) otaBegun = true;
      else { otaOk = false; otaErr = "spazio insufficiente"; }
    }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (otaOk) {
      if (otaFirst) {
        otaFirst = false;
        if (u.buf[0] != 0xE9) { otaOk = false; otaErr = "file non valido"; }
      }
      if (otaOk && Update.write(u.buf, u.currentSize) != u.currentSize) {
        otaOk = false;
        otaErr = "scrittura fallita";
      }
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (otaOk) {
      if (!Update.end(true)) { otaOk = false; otaErr = "immagine non valida"; }
    } else if (otaBegun) {
      Update.abort();
    }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (otaBegun) Update.abort();
    otaOk = false;
    otaErr = "interrotto";
  }
}

void handleUpdateDone() {
  bool ok = otaOk && !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain; charset=utf-8", ok ? String("OK") : (otaErr.length() ? otaErr : String("errore")));
  if (ok) {
    otaScreen("Fatto!", "Riavvio...");
    delay(800);
    ESP.restart();
  } else {
    gPortal = false;
    needFull = true;
  }
}

bool srvUp = false;

void startServer() {
  if (srvUp) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tasto", HTTP_GET, handleKey);
  server.on("/stato", HTTP_GET, handleState);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  srvUp = true;
}

// ------------------------------------------------------ Portale WiFi --------
void runPortal() {
  gPortal = true;   // ferma il task dati mentre il portale e' attivo
  tft.fillScreen(C_BG);
  drawStr("Configurazione", 120, 14, 4, 1, C_ACC, C_BG);
  drawStr("Cerco le reti WiFi...", 120, 70, 2, 1, C_DIM, C_BG);

  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  netOpts = "";
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || netOpts.indexOf("'" + htmlEsc(s) + "'") >= 0) continue;
    netOpts += "<option value='" + htmlEsc(s) + "'>" + htmlEsc(s) + "</option>";
  }
  WiFi.softAP(AP_NAME);
  delay(300);
  dns.start(53, "*", WiFi.softAPIP());
  startServer();

  tft.fillScreen(C_BG);
  drawStr("Configurazione", 120, 14, 4, 1, C_ACC, C_BG);
  drawStr("1. Collegati al WiFi", 120, 64, 2, 1, C_TEXT, C_BG);
  drawStr(AP_NAME, 120, 86, 4, 1, C_ACC2, C_BG);
  drawStr("2. Apri il browser su", 120, 134, 2, 1, C_TEXT, C_BG);
  drawStr("192.168.4.1", 120, 156, 4, 1, C_ACC2, C_BG);
  drawStr("Imposta rete, citta e ora", 120, 204, 2, 1, C_DIM, C_BG);

  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }
}

// ----------------------------------------------------- Gestione schermate ---
void drawFull() {
  if (!inFunc) {
    menuFull();
  } else if (menuIdx == 0) {
    if (funcIdx == 0) meteoFull();
    else if (funcIdx == 1) previsioniFull();
    else graficoFull();
  } else {
    if (funcIdx == 0) airFull();
    else healthFull();
  }
}

void handleGesture(Gesture g) {
  if (!inFunc) {
    if (g == G_SINGLE) menuIdx = (menuIdx + 1) % NMENU;
    else if (g == G_DOUBLE) menuIdx = (menuIdx + NMENU - 1) % NMENU;
    else if (g == G_LONG) {
      inFunc = true;
      funcIdx = 0;
    }
  } else {
    int n = funcCount(menuIdx);
    if (g == G_SINGLE) funcIdx = (funcIdx + 1) % n;
    else if (g == G_DOUBLE) funcIdx = (funcIdx + n - 1) % n;
    else if (g == G_LONG) inFunc = false;
  }
  needFull = true;
}

void updateScreen() {
  if (needFull) {
    needFull = false;
    syncData();
    drawFull();
    return;
  }
  bool changed = syncData();
  if (!inFunc) {
#ifdef TOUCH_DEBUG
    static uint32_t dbgAt = 0;
    if (millis() - dbgAt > 150) {
      dbgAt = millis();
      char b[48];
      snprintf(b, sizeof b, "T:%u base:%.0f on:%u %s", touchRaw, touchBase, thrOn, tState ? "PREMUTO" : "");
      tft.fillRect(0, 224, 240, 16, C_BG);
      drawStr(b, 120, 224, 2, 1, tState ? C_ACC2 : C_DIM, C_BG);
    }
#endif
  } else if (menuIdx == 0 && funcIdx == 0) {
    meteoTick(changed);
  } else if (changed) {
    drawFull();
  }
}

// -------------------------------------------------------------- setup -------
void setBacklight(uint8_t level) { ledcWrite(0, 255 - level); }

void setup() {
  Serial.begin(115200);
  delay(50);
  loadCfg();
  applyTheme(cfg.theme);

  Serial.printf("\n=== Small TV Evolution Pro v%s ===\n", FW_VERSION);
  pinMode(PIN_PANEL_PWR, OUTPUT);
  digitalWrite(PIN_PANEL_PWR, LOW);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BL, 0);
  setBacklight(0);
  delay(300);                       // il pannello ha bisogno di tempo dopo l'accensione

  tft.init();
  tft.setRotation(cfg.rot ? 2 : 0);
  tft.invertDisplay(cfg.inv);
  tft.fillScreen(C_BG);
  setBacklight(255);
  Serial.println("Display ok");

  mtx = xSemaphoreCreateMutex();

  if (cfg.ssid.length() == 0) runPortal();   // primo avvio: portale di configurazione

  inSplash = true;
  splashStart = millis();
  splashStatic();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("nmtv");
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  configTzTime(TZ_ROME, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  calibrateTouch();
  startServer();
  xTaskCreatePinnedToCore(dataTask, "dati", 16384, nullptr, 1, nullptr, 0);

  menuIdx = 0;
  inFunc = false;
  needFull = true;
  warmUntil = millis() + 6000;
}

void loop() {
  static bool mdnsOk = false;
  if (!mdnsOk && WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("nmtv")) MDNS.addService("http", "tcp", 80);
    mdnsOk = true;
  }
  server.handleClient();

  if (inSplash) {                 // titolo e indirizzo IP: non toccare il pulsante
    touchPressed();               // intanto il sensore si assesta
    splashTick();
    delay(10);
    return;
  }

  Gesture g = readGesture();
  if (g == G_VLONG) {
    runPortal();                // non ritorna
  } else if (g != G_NONE) {
    Serial.printf("Gesto: %d\n", (int)g);
    handleGesture(g);
  }
  updateScreen();
  delay(10);
}
