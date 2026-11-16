// ============================================================================
//  NM-TV Meteo & Aria  -  firmware per NM-TV-154 (ESP32-WROOM-32E)
//
//  Menu di schermate comandato dal pulsante touch superiore (GPIO32 / T9):
//    MENU   : 1 tocco = avanti, 2 tocchi = indietro, tieni premuto = entra
//    DENTRO : 1 tocco = funzione successiva, 2 tocchi = funzione precedente,
//             tieni premuto = torna al menu
//    Tieni premuto 6 secondi = portale di configurazione WiFi/citta'
//
//  Schermate:  Meteo (principale, previsioni 3 giorni)  |  Aria (EAQI, PM2.5/PM10)
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
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <math.h>

#define FW_VERSION "1.0.1"

// ---------------------------------------------------------------- Pin -------
#define PIN_BL        19   // retroilluminazione, PWM, attiva bassa
#define PIN_PANEL_PWR 21   // alimentazione pannello, va portato a LOW
#define PIN_TOUCH     32   // pulsante touch (canale T9)
#define AP_NAME       "NM-TV-Setup"

// ------------------------------------------------------------- Colori -------
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static const uint16_t C_BG     = RGB565(8, 12, 28);
static const uint16_t C_CARD   = RGB565(22, 32, 60);
static const uint16_t C_TRACK  = RGB565(38, 48, 74);
static const uint16_t C_TEXT   = 0xFFFF;
static const uint16_t C_DIM    = RGB565(140, 152, 180);
static const uint16_t C_ACC    = RGB565(90, 200, 255);
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
  bool geo = false, fmt12 = false, inv = false, rot = false;
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
  cfg.inv   = prefs.getBool("inv", false);
  cfg.rot   = prefs.getBool("rot", false);
  prefs.end();
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
  time_t upd = 0;
};
struct Air {
  bool valid = false;
  float aqi = 0, pm25 = 0, pm10 = 0;
  time_t upd = 0;
};

enum { ST_WIFI, ST_GEO, ST_FETCH, ST_OK, ST_ERR_GEO, ST_ERR_NET };

SemaphoreHandle_t mtx;
Weather Wsh, W;          // Wsh = scritto dal task dati, W = copia per il display
Air Ash, A;
volatile uint32_t gVer = 0;
volatile int gStatus = ST_WIFI;
volatile bool gPortal = false;
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
               "&current=european_aqi,pm10,pm2_5&timezone=Europe%2FRome";
  JsonDocument doc;
  if (!getJson(url, doc)) return false;
  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;

  Air a;
  a.valid = true;
  a.aqi  = cur["european_aqi"] | 0.0f;
  a.pm25 = cur["pm2_5"] | 0.0f;
  a.pm10 = cur["pm10"] | 0.0f;
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
static const uint32_t VLONG_MS   = 6000;
static const uint32_t DOUBLE_MS  = 350;
static const uint32_t MIN_TAP_MS = 30;

float touchBase = 60;
uint16_t thrOn = 43, thrOff = 49;
bool tState = false;

uint16_t readTouch() {
  uint16_t a = touchRead(PIN_TOUCH), b = touchRead(PIN_TOUCH), c = touchRead(PIN_TOUCH);
  uint16_t lo = a < b ? a : b;
  uint16_t hi = a < b ? b : a;
  uint16_t mid = c < hi ? c : hi;
  return mid > lo ? mid : lo;   // mediana di 3 letture
}

void calibrateTouch() {
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += touchRead(PIN_TOUCH);
    delay(8);
  }
  touchBase = sum / 32.0f;
  thrOn = (uint16_t)(touchBase * 0.72f);
  thrOff = (uint16_t)(touchBase * 0.82f);
  Serial.printf("Touch: base=%.0f soglia on=%u off=%u\n", touchBase, thrOn, thrOff);
}

bool touchPressed() {
  uint16_t v = readTouch();
  if (!tState && v < thrOn) tState = true;
  else if (tState && v > thrOff) tState = false;
  if (!tState && v > thrOn) {            // segue lentamente la deriva a riposo
    touchBase += ((float)v - touchBase) / 128.0f;
    thrOn = (uint16_t)(touchBase * 0.72f);
    thrOff = (uint16_t)(touchBase * 0.82f);
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

int funcCount(int m) { return m == 0 ? 2 : 1; }

String cityName() { return (cfg.geo && cfg.city.length()) ? cfg.city : cfg.cityq; }

// ------------------------------------------------------------- MENU ---------
String lastIp = "";

void drawMenuFooter() {
  tft.fillRect(0, 222, 240, 18, C_BG);
  String s;
  if (WiFi.status() == WL_CONNECTED) {
    lastIp = WiFi.localIP().toString();
    s = "nmtv.local  " + lastIp;
  } else {
    lastIp = "";
    s = "WiFi non connesso";
  }
  drawStr(s, 120, 224, 2, 1, RGB565(90, 100, 130), C_BG);
}

void menuFull() {
  tft.fillScreen(C_BG);
  const char* title = (menuIdx == 0) ? "Meteo" : "Aria";
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(title, 120, 8, 4);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  if (menuIdx == 0) drawIcon(120, 120, IC_PARTLY, true, 1.5f, C_BG);
  else iconWind(120, 118, 1.25f, C_AIR, RGB565(150, 235, 255), C_BG);

  for (int i = 0; i < NMENU; i++) {
    tft.fillCircle(110 + 20 * i, 178, 4, i == menuIdx ? C_ACC : C_TRACK);
  }
  drawStr("1 tocco: avanti | 2: indietro", 120, 188, 2, 1, C_DIM, C_BG);
  drawStr("Tieni premuto: entra", 120, 204, 2, 1, C_DIM, C_BG);
  drawMenuFooter();
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
  drawStr(b, 120, 4, 2, 1, C_ACC, C_BG);
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

void drawWeatherBlock() {
  tft.fillRect(0, 90, 240, 98, C_BG);
  if (!W.valid) {
    drawIcon(46, 130, IC_CLOUD, true, 1.0f, C_BG);
    drawStr("--", 100, 118, 4, 0, C_DIM, C_BG);
    return;
  }
  drawIcon(46, 130, iconFor(W.code), W.day, 1.0f, C_BG);

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

  int x = 8;
  x += drawStr("Min " + String((int)lroundf(W.tmin[0])) + "~", x, 170, 2, 0, C_MIN, C_BG) + 12;
  x += drawStr("Max " + String((int)lroundf(W.tmax[0])) + "~", x, 170, 2, 0, C_MAX, C_BG) + 12;
  drawStr("Umid. " + String((int)lroundf(W.hum)) + "%", x, 170, 2, 0, C_ACC, C_BG);
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
  drawStr("Prossimi 3 giorni", 120, 3, 2, 1, C_ACC, C_BG);
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

void pmRow(int y, const char* label, float v, float scale, const float* th) {
  int band = 0;
  while (band < 5 && v >= th[band]) band++;
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

// ------------------------------------------------------ Pagina impostazioni -
String buildPage(bool ap) {
  String h;
  h.reserve(4500);
  h += F("<!DOCTYPE html><html lang='it'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'><title>NM-TV</title><style>"
         "body{font-family:system-ui,sans-serif;background:#0b1020;color:#e8eefc;margin:0 auto;padding:18px;max-width:480px}"
         "h1{font-size:22px}label{display:block;margin:14px 0 4px;color:#9fb0d0;font-size:14px}"
         "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #33406a;"
         "background:#141c36;color:#fff;font-size:16px}"
         "button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:10px;background:#3aa0ff;color:#fff;"
         "font-size:17px;font-weight:600}.c{display:flex;align-items:center;gap:10px;margin-top:14px}"
         ".c input{width:auto}small{color:#7d8db3;display:block;margin-top:6px}"
         "</style></head><body><h1>NM-TV Meteo &amp; Aria</h1><form method='POST' action='/save'>");

  h += F("<label>Rete WiFi</label>");
  if (ap && netOpts.length()) {
    h += F("<select name='ssid'><option value=''>-- scegli una rete --</option>");
    h += netOpts;
    h += F("</select><label>oppure scrivi il nome della rete</label>");
  }
  h += "<input name='ssid_manual' autocapitalize='off' placeholder='" + htmlEsc(cfg.ssid) + "'>";
  h += F("<label>Password WiFi</label><input name='pass' type='password' placeholder='lascia vuoto per non cambiarla'>");
  h += "<label>Citt&agrave;</label><input name='cityq' value='" + htmlEsc(cfg.cityq) + "'>";
  h += F("<small>Scrivi il nome (es. Roma, Chieti, Milano). Le coordinate si trovano da sole.</small>");
  h += F("<label>Formato ora</label><select name='fmt'><option value='24'");
  if (!cfg.fmt12) h += F(" selected");
  h += F(">24 ore</option><option value='12'");
  if (cfg.fmt12) h += F(" selected");
  h += F(">12 ore (AM/PM)</option></select>");
  h += F("<div class='c'><input type='checkbox' name='inv' id='inv'");
  if (cfg.inv) h += F(" checked");
  h += F("><label for='inv' style='margin:0'>Inverti i colori (se lo schermo sembra un negativo)</label></div>");
  h += F("<div class='c'><input type='checkbox' name='rot' id='rot'");
  if (cfg.rot) h += F(" checked");
  h += F("><label for='rot' style='margin:0'>Ruota lo schermo di 180&deg;</label></div>");
  h += F("<button type='submit'>Salva e riavvia</button></form><small>Firmware ");
  h += FW_VERSION;
  h += F("</small></body></html>");
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
  cfg.inv = server.hasArg("inv");
  cfg.rot = server.hasArg("rot");
  saveCfg();
  server.send(200, "text/html; charset=utf-8",
              String("<!DOCTYPE html><html lang='it'><head><meta charset='utf-8'>"
                     "<meta name='viewport' content='width=device-width,initial-scale=1'></head>"
                     "<body style='font-family:system-ui;background:#0b1020;color:#e8eefc;padding:30px'>"
                     "<h2>Salvato!</h2><p>Il dispositivo si riavvia...</p></body></html>"));
  delay(1500);
  ESP.restart();
}

bool srvUp = false;

void startServer() {
  if (srvUp) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
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
  drawStr(AP_NAME, 120, 86, 4, 1, C_SUN, C_BG);
  drawStr("2. Apri il browser su", 120, 134, 2, 1, C_TEXT, C_BG);
  drawStr("192.168.4.1", 120, 156, 4, 1, C_SUN, C_BG);
  drawStr("Imposta rete, citta e ora", 120, 204, 2, 1, C_DIM, C_BG);

  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }
}

// ----------------------------------------------------- Gestione schermate ---
void drawFull() {
  if (!inFunc) menuFull();
  else if (menuIdx == 0) {
    if (funcIdx == 0) meteoFull();
    else previsioniFull();
  } else {
    airFull();
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
    String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
    if (ip != lastIp) drawMenuFooter();
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

  Serial.printf("\n=== NM-TV Meteo & Aria v%s ===\n", FW_VERSION);
  Serial.println("1) alimento il pannello (GPIO21 LOW)");
  pinMode(PIN_PANEL_PWR, OUTPUT);
  digitalWrite(PIN_PANEL_PWR, LOW);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BL, 0);
  setBacklight(0);
  delay(300);                       // il pannello ha bisogno di tempo dopo l'accensione

  Serial.println("2) inizializzo il display");
  tft.init();
  tft.setRotation(cfg.rot ? 2 : 0);
  tft.invertDisplay(cfg.inv);
  setBacklight(255);
  Serial.println("3) test colori: rosso, verde, blu");
  tft.fillScreen(TFT_RED);   delay(400);
  tft.fillScreen(TFT_GREEN); delay(400);
  tft.fillScreen(TFT_BLUE);  delay(400);
  tft.fillScreen(C_BG);
  Serial.println("4) display ok");

  mtx = xSemaphoreCreateMutex();

  if (cfg.ssid.length() == 0) runPortal();   // primo avvio: portale di configurazione

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
}

void loop() {
  static bool mdnsOk = false;
  if (!mdnsOk && WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("nmtv")) MDNS.addService("http", "tcp", 80);
    mdnsOk = true;
  }
  server.handleClient();

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
