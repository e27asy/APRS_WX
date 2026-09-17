#include <Arduino.h>

/****************************************************************
 *  ESP32 APRS Weather Beacon + OLED 0.96" (SSD1306 128x64 I2C)
 *  v2.3  -  production build
 *
 *  Features
 *   - APRS position beacon (rally comment + live weather summary)
 *   - APRS complete weather report (spec-compliant, imperial)
 *   - Weather: Open-Meteo (primary) -> WeatherAPI.com (fallback)
 *   - Auto failover + 30-min cooldown + stale-data guard
 *   - NTP zulu timestamp
 *   - Random LED blink 3-5s (non-blocking)
 *   - OLED 3-page auto rotate, auto-fit font, highlight header
 *
 *  Wiring OLED: GND->GND  VCC->3V3  SCL->GPIO22  SDA->GPIO21
 ****************************************************************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <math.h>

// =====================
// Wi-Fi Configuration
// =====================
const char* WIFI_SSID     = "e27cyf_2.4GHz";
const char* WIFI_PASSWORD = "0896066266";

// =====================
// APRS Configuration
// =====================
const char* APRS_SERVER = "asia.aprs2.net";
const int   APRS_PORT   = 14580;

const char* CALLSIGN = "E24FG";
const char* PASSCODE = "17814";

const char* LATITUDE  = "1447.03N";
const char* LONGITUDE = "10040.63E";

const char* COMMENT_BASE = "145.6625MHz dup-  ASL node 61780";
const char* SOFTWARE_ID  = "INDY";

const unsigned long BEACON_INTERVAL_MS = 60UL * 1000UL;

// =====================
// Weather sources (เมือง, ลพบุรี)
// =====================
const char* WX_LAT = "14.7995";

const char* WX_LON = "100.6534";

const char* WAPI_KEY = "e2cea38743f84cb582245116261709";

const unsigned long WEATHER_INTERVAL_MS = 5UL  * 60UL * 1000UL;
const unsigned long OM_COOLDOWN_MS      = 30UL * 60UL * 1000UL;
const uint8_t       OM_FAIL_LIMIT       = 3;
const unsigned long WX_MAX_AGE_MS       = 60UL * 60UL * 1000UL;

// =====================
// NTP
// =====================
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.google.com";

// =====================
// LED
// =====================
const int  LED_PIN    = 2;
const int  LED_ON_MS  = 120;
const long LED_MIN_MS = 3000;
const long LED_MAX_MS = 5000;

// =====================
// OLED
// =====================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
#define I2C_SDA       21
#define I2C_SCL       22

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const unsigned long OLED_PAGE_MS = 4000;
bool oledOk = false;

// =====================
// Global state
// =====================
unsigned long lastBeaconTime  = 0;
unsigned long lastWeatherTime = 0;
unsigned long lastBlinkTime   = 0;
unsigned long lastPageTime    = 0;
unsigned long blinkInterval   = 4000;
bool          ledState        = false;
uint8_t       oledPage        = 0;
uint32_t      beaconCount     = 0;

uint8_t       omFailCount     = 0;
bool          omCoolingDown   = false;
unsigned long omCooldownStart = 0;
unsigned long lastWxSuccess   = 0;
const char*   wxSource        = "-";

bool   wxValid   = false;
String wxText    = "N/A";
float  wxTempC   = NAN;
float  wxTempF   = NAN;
int    wxHum     = -1;
int    wxWindDir = -1;
float  wxWindKmh = NAN;
float  wxWindMph = NAN;
float  wxGustMph = NAN;
float  wxPresMb  = NAN;
float  wxRain1hIn  = NAN;
float  wxRain24hIn = NAN;


// =====================
// Wi-Fi
// =====================
bool connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Wi-Fi connection failed.");
  return false;
}


// =====================
// HTTP helper
// =====================
bool httpGetString(const String& url, String& out)
{
  HTTPClient http;
  http.setTimeout(12000);
  http.setConnectTimeout(8000);

  if (!http.begin(url)) {
    Serial.println("  http.begin() failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("  HTTP error: ");
    Serial.println(code);
    http.end();
    return false;
  }

  out = http.getString();
  http.end();
  return out.length() > 0;
}


// =====================
// WMO code -> text
// =====================
static const char* wmoToText(int code)
{
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mostly Clear";
    case 2:  return "Partly Cloudy";
    case 3:  return "Overcast";
    case 45: case 48:            return "Fog";
    case 51: case 53: case 55:   return "Drizzle";
    case 56: case 57:            return "Freezing Drizzle";
    case 61:                     return "Light Rain";
    case 63:                     return "Rain";
    case 65:                     return "Heavy Rain";
    case 66: case 67:            return "Freezing Rain";
    case 71: case 73: case 75:   return "Snow";
    case 77:                     return "Snow Grains";
    case 80:                     return "Rain Showers";
    case 81:                     return "Showers";
    case 82:                     return "Heavy Showers";
    case 85: case 86:            return "Snow Showers";
    case 95:                     return "Thunderstorm";
    case 96: case 99:            return "Thunderstorm Hail";
    default:                     return "N/A";
  }
}


// =====================
// SOURCE 1: Open-Meteo (no API key)
// =====================
bool fetchOpenMeteo()
{
  Serial.println("[WX] Trying Open-Meteo ...");

  String url = "http://api.open-meteo.com/v1/forecast";
  url += "?latitude=";  url += WX_LAT;
  url += "&longitude="; url += WX_LON;
  url += "&current=temperature_2m,relative_humidity_2m,pressure_msl,"
         "wind_speed_10m,wind_direction_10m,wind_gusts_10m,precipitation,weather_code";
  url += "&daily=precipitation_sum";
  url += "&forecast_days=1";
  url += "&timezone=Asia%2FBangkok";
  url += "&temperature_unit=fahrenheit";
  url += "&wind_speed_unit=mph";
  url += "&precipitation_unit=inch";

  String payload;
  if (!httpGetString(url, payload)) return false;

  StaticJsonDocument<384> filter;
  filter["current"]["temperature_2m"]       = true;
  filter["current"]["relative_humidity_2m"] = true;
  filter["current"]["pressure_msl"]         = true;
  filter["current"]["wind_speed_10m"]       = true;
  filter["current"]["wind_direction_10m"]   = true;
  filter["current"]["wind_gusts_10m"]       = true;
  filter["current"]["precipitation"]        = true;
  filter["current"]["weather_code"]         = true;
  filter["daily"]["precipitation_sum"]      = true;

  DynamicJsonDocument doc(1024);
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.print("  JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  if (cur.isNull() || cur["temperature_2m"].isNull()) {
    Serial.println("  No usable 'current' data.");
    return false;
  }

  wxTempF    = cur["temperature_2m"]       | NAN;
  wxHum      = cur["relative_humidity_2m"] | -1;
  wxPresMb   = cur["pressure_msl"]         | NAN;
  wxWindMph  = cur["wind_speed_10m"]       | NAN;
  wxWindDir  = cur["wind_direction_10m"]   | -1;
  wxGustMph  = cur["wind_gusts_10m"]       | NAN;
  wxRain1hIn = cur["precipitation"]        | NAN;

  wxRain24hIn = doc["daily"]["precipitation_sum"][0] | NAN;

  wxTempC   = isnan(wxTempF)   ? NAN : (wxTempF - 32.0f) * 5.0f / 9.0f;
  wxWindKmh = isnan(wxWindMph) ? NAN : wxWindMph * 1.609344f;

  wxText = wmoToText(cur["weather_code"] | -1);

  Serial.println("[WX] Open-Meteo OK");
  return true;
}


// =====================
// SOURCE 2: WeatherAPI.com (fallback)
// =====================
bool fetchWeatherAPI()
{
  Serial.println("[WX] Trying WeatherAPI.com ...");

  if (strlen(WAPI_KEY) < 10 ||
      strcmp(WAPI_KEY, "PUT_YOUR_WEATHERAPI_KEY_HERE") == 0) {
    Serial.println("  WeatherAPI key not set. Skipped.");
    return false;
  }

  String url = "http://api.weatherapi.com/v1/forecast.json";
  url += "?key="; url += WAPI_KEY;
  url += "&q=";   url += WX_LAT; url += ","; url += WX_LON;
  url += "&days=1&aqi=no&alerts=no";

  String payload;
  if (!httpGetString(url, payload)) return false;

  StaticJsonDocument<512> filter;
  JsonObject fc = filter.createNestedObject("current");
  fc["temp_c"]      = true;
  fc["temp_f"]      = true;
  fc["humidity"]    = true;
  fc["pressure_mb"] = true;
  fc["wind_mph"]    = true;
  fc["wind_kph"]    = true;
  fc["wind_degree"] = true;
  fc["gust_mph"]    = true;
  fc["precip_in"]   = true;
  fc["condition"]["text"] = true;
  filter["forecast"]["forecastday"][0]["day"]["totalprecip_in"] = true;

  DynamicJsonDocument doc(1536);
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.print("  JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  if (cur.isNull() || cur["temp_f"].isNull()) {
    Serial.println("  No usable 'current' data.");
    return false;
  }

  wxTempC    = cur["temp_c"]      | NAN;
  wxTempF    = cur["temp_f"]      | NAN;
  wxHum      = cur["humidity"]    | -1;
  wxPresMb   = cur["pressure_mb"] | NAN;
  wxWindMph  = cur["wind_mph"]    | NAN;
  wxWindKmh  = cur["wind_kph"]    | NAN;
  wxWindDir  = cur["wind_degree"] | -1;
  wxGustMph  = cur["gust_mph"]    | NAN;
  wxRain1hIn = cur["precip_in"]   | NAN;

  wxRain24hIn = doc["forecast"]["forecastday"][0]["day"]["totalprecip_in"] | NAN;

  const char* t = cur["condition"]["text"] | "N/A";
  wxText = String(t);

  Serial.println("[WX] WeatherAPI.com OK");
  return true;
}


// =====================
// Failover manager
// =====================
bool fetchWeather()
{
  if (!connectWiFi()) return false;

  unsigned long now = millis();

  if (omCoolingDown && (now - omCooldownStart >= OM_COOLDOWN_MS)) {
    omCoolingDown = false;
    omFailCount   = 0;
    Serial.println("[WX] Open-Meteo cooldown ended, will retry.");
  }

  bool ok = false;

  if (!omCoolingDown) {
    ok = fetchOpenMeteo();
    if (ok) {
      omFailCount = 0;
      wxSource    = "Open-Meteo";
    } else {
      omFailCount++;
      Serial.print("[WX] Open-Meteo fail #");
      Serial.println(omFailCount);
      if (omFailCount >= OM_FAIL_LIMIT) {
        omCoolingDown   = true;
        omCooldownStart = now;
        Serial.println("[WX] Open-Meteo entering 30-min cooldown.");
      }
    }
  } else {
    Serial.println("[WX] Open-Meteo in cooldown, skipping.");
  }

  if (!ok) {
    ok = fetchWeatherAPI();
    if (ok) wxSource = "WeatherAPI";
  }

  if (ok) {
    wxValid       = true;
    lastWxSuccess = millis();
  } else {
    Serial.println("[WX] All sources failed. Keeping last known data.");
  }
  return ok;
}


bool wxIsStale()
{
  if (!wxValid || lastWxSuccess == 0) return true;
  return (millis() - lastWxSuccess) >= WX_MAX_AGE_MS;
}


void printWeather()
{
  Serial.println("---- Weather: meung, lopburi ----");
  if (!wxValid) {
    Serial.println("  No data yet.");
    Serial.println("----------------------------------");
    return;
  }
  Serial.print("  Source    : "); Serial.print(wxSource);
  Serial.print(" (age ");         Serial.print((millis() - lastWxSuccess) / 1000);
  Serial.println("s)");
  Serial.print("  Condition : "); Serial.println(wxText);
  Serial.print("  Temp      : "); Serial.print(wxTempC, 1);
  Serial.print(" C / ");          Serial.print(wxTempF, 1); Serial.println(" F");
  Serial.print("  Humidity  : "); Serial.print(wxHum);       Serial.println(" %");
  Serial.print("  Wind      : "); Serial.print(wxWindDir);
  Serial.print(" deg @ ");        Serial.print(wxWindKmh, 1); Serial.println(" km/h");
  Serial.print("  Gust      : "); Serial.print(wxGustMph, 1); Serial.println(" mph");
  Serial.print("  Pressure  : "); Serial.print(wxPresMb, 1);  Serial.println(" mb");
  Serial.print("  Rain 1h   : "); Serial.print(wxRain1hIn, 2);  Serial.println(" in");
  Serial.print("  Rain today: "); Serial.print(wxRain24hIn, 2); Serial.println(" in");
  Serial.println("----------------------------------");
}


// =====================
// APRS formatting helpers
// =====================
static String fmt3(int v)
{
  if (v < 0)   v = 0;
  if (v > 999) v = 999;
  char b[8]; snprintf(b, sizeof(b), "%03d", v); return String(b);
}

static String fmtTempF(float f)
{
  if (isnan(f)) return "t...";
  int t = (int)lroundf(f);
  char b[8];
  if (t < 0) { if (t < -99) t = -99; snprintf(b, sizeof(b), "t-%02d", -t); }
  else       { if (t > 999) t = 999; snprintf(b, sizeof(b), "t%03d", t);  }
  return String(b);
}

static String fmtRain(char id, float inches)
{
  char b[8];
  if (isnan(inches)) { snprintf(b, sizeof(b), "%c...", id); return String(b); }
  int h = (int)lroundf(inches * 100.0f);
  if (h < 0)   h = 0;
  if (h > 999) h = 999;
  snprintf(b, sizeof(b), "%c%03d", id, h);
  return String(b);
}

static String fmtHum(int h)
{
  if (h < 0)    return "h..";
  if (h >= 100) return "h00";
  char b[8]; snprintf(b, sizeof(b), "h%02d", h); return String(b);
}

static String fmtPressure(float mb)
{
  if (isnan(mb)) return "b.....";
  int p = (int)lroundf(mb * 10.0f);
  if (p < 0)     p = 0;
  if (p > 99999) p = 99999;
  char b[10]; snprintf(b, sizeof(b), "b%05d", p); return String(b);
}

static String zuluTimestamp()
{
  time_t now = time(nullptr);
  struct tm t;
  if (now < 1600000000 || !gmtime_r(&now, &t)) return "";
  char b[12];
  snprintf(b, sizeof(b), "%02d%02d%02dz", t.tm_mday, t.tm_hour, t.tm_min);
  return String(b);
}


// =====================
// Packet builders
// =====================
String buildWeatherPacket()
{
  String ts = zuluTimestamp();

  String p = CALLSIGN;
  p += ">APRS,TCPIP*:";

  if (ts.length() > 0) { p += "@"; p += ts; }
  else                 { p += "!"; }

  p += LATITUDE;
  p += "/";
  p += LONGITUDE;
  p += "_";

  p += (wxWindDir >= 0) ? fmt3(wxWindDir) : String("...");
  p += "/";
  p += isnan(wxWindMph) ? String("...") : fmt3((int)lroundf(wxWindMph));
  p += "g";
  p += isnan(wxGustMph) ? String("...") : fmt3((int)lroundf(wxGustMph));

  p += fmtTempF(wxTempF);
  p += fmtRain('r', wxRain1hIn);
  p += fmtRain('p', wxRain24hIn);
  p += fmtRain('P', wxRain24hIn);
  p += fmtHum(wxHum);
  p += fmtPressure(wxPresMb);

  p += SOFTWARE_ID;
  p += "\r\n";
  return p;
}

String buildPositionPacket()
{
  String c = COMMENT_BASE;
  if (wxValid) {
    c += " | Lopburi ";
    c += String(wxTempC, 1);   c += "C ";
    c += String(wxHum);        c += "% ";
    c += String(wxWindKmh, 0); c += "km/h ";
    c += wxText;
    if (wxIsStale()) c += " (stale)";
  }
  if (c.length() > 180) c = c.substring(0, 180);

  String p = CALLSIGN;
  p += ">APRS,TCPIP*:=";
  p += LATITUDE;
  p += "/";
  p += LONGITUDE;
  p += "- ";
  p += c;
  p += "\r\n";
  return p;
}


// =====================
// APRS-IS transmit
// =====================
bool sendAprsBeacon()
{
  if (!connectWiFi()) return false;

  WiFiClient client;
  client.setNoDelay(true);

  Serial.print("Connecting APRS-IS: ");
  Serial.print(APRS_SERVER); Serial.print(":"); Serial.println(APRS_PORT);

  if (!client.connect(APRS_SERVER, APRS_PORT)) {
    Serial.println("APRS-IS connection failed.");
    return false;
  }

  delay(400);
  while (client.available()) {
    String l = client.readStringUntil('\n'); l.trim();
    if (l.length()) { Serial.print("Server: "); Serial.println(l); }
  }

  String login = "user ";
  login += CALLSIGN; login += " pass "; login += PASSCODE;
  login += " vers ESP32WxBeacon 2.3\r\n";
  Serial.print("Login: "); Serial.print(login);
  client.print(login);

  delay(400);
  while (client.available()) {
    String l = client.readStringUntil('\n'); l.trim();
    if (l.length()) { Serial.print("Server: "); Serial.println(l); }
  }

  String pos = buildPositionPacket();
  Serial.print("TX POS: "); Serial.print(pos);
  client.print(pos);
  delay(600);

  if (wxValid && !wxIsStale()) {
    String wx = buildWeatherPacket();
    Serial.print("TX WX : "); Serial.print(wx);
    client.print(wx);
    delay(600);
  } else if (wxValid) {
    Serial.println("WX data too old, skipping weather packet.");
  }

  client.stop();
  beaconCount++;
  Serial.println("Beacon sent, connection closed.");
  return true;
}


// =====================
// LED (non-blocking)
// =====================
void handleLed()
{
  unsigned long now = millis();

  if (!ledState && (now - lastBlinkTime >= blinkInterval)) {
    ledState = true;
    digitalWrite(LED_PIN, HIGH);
    lastBlinkTime = now;
  }
  else if (ledState && (now - lastBlinkTime >= (unsigned long)LED_ON_MS)) {
    ledState = false;
    digitalWrite(LED_PIN, LOW);
    lastBlinkTime = now;
    blinkInterval = random(LED_MIN_MS, LED_MAX_MS + 1);
  }
}


// =====================
// OLED text helpers
// =====================
void drawHeaderBar(const char* title, bool alert = false)
{
  display.fillRect(0, 0, SCREEN_WIDTH, 11, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);

  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - (int)w) / 2, 2);
  display.print(title);

  if (alert) display.fillCircle(SCREEN_WIDTH - 6, 5, 2, SSD1306_BLACK);

  display.setTextColor(SSD1306_WHITE);
}

uint8_t fitTextSize(const String& s, uint8_t maxSize, int maxW = SCREEN_WIDTH)
{
  for (uint8_t sz = maxSize; sz > 1; sz--) {
    if ((int)s.length() * 6 * sz <= maxW) return sz;
  }
  return 1;
}

void drawCentered(const String& s, int y, uint8_t size)
{
  display.setTextSize(size);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - (int)w) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, y);
  display.print(s);
}

void drawRow(const char* label, const String& val, int y)
{
  display.setTextSize(1);
  display.setCursor(0, y);
  display.print(label);

  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(val, 0, y, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - (int)w, y);
  display.print(val);
}


// =====================
// OLED init + pages
// =====================
void oledInit()
{
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOk) {
    Serial.println("SSD1306 not found. Try OLED_ADDR 0x3D or check wiring.");
    return;
  }

  display.clearDisplay();
  drawHeaderBar("ESP32 APRS WX");
  drawCentered(String(CALLSIGN), 20, fitTextSize(String(CALLSIGN), 3));
  drawCentered("v2.3 booting...", 50, 1);
  display.display();
  delay(1200);
}

void oledPageStatus()
{
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  drawHeaderBar("STATUS", !wifiUp);

  String cs = String(CALLSIGN);
  drawCentered(cs, 15, fitTextSize(cs, 3));

  display.setTextColor(SSD1306_WHITE);
  drawRow("WiFi", wifiUp ? "OK" : "DOWN", 40);
  drawRow("Src",  String(wxSource),        49);

  unsigned long left = 0;
  unsigned long el = millis() - lastBeaconTime;
  if (el < BEACON_INTERVAL_MS) left = (BEACON_INTERVAL_MS - el) / 1000;
  drawRow("TX", String(beaconCount) + "  " + String(left) + "s", 57);
}

void oledPageWeather()
{
  bool stale = (wxValid && wxIsStale());
  drawHeaderBar("LAK SI WX", stale);

  if (!wxValid) {
    drawCentered("NO DATA", 28, 2);
    return;
  }

  String temp = String(wxTempC, 1) + "C";
  drawCentered(temp, 14, fitTextSize(temp, 3));

  display.setTextColor(SSD1306_WHITE);
  drawRow("RH",   String(wxHum) + "%",                                41);
  drawRow("Wind", String(wxWindKmh, 0) + "km/h " + String(wxWindDir), 49);
  drawRow("Baro", String(wxPresMb, 1) + "mb",                         57);
}

void oledPagePacket()
{
  drawHeaderBar("WX PACKET");

  if (!wxValid) {
    drawCentered("NO DATA", 28, 2);
    return;
  }

  String d = "_";
  d += (wxWindDir >= 0) ? fmt3(wxWindDir) : String("...");
  d += "/";
  d += isnan(wxWindMph) ? String("...") : fmt3((int)lroundf(wxWindMph));
  d += "g";
  d += isnan(wxGustMph) ? String("...") : fmt3((int)lroundf(wxGustMph));
  d += fmtTempF(wxTempF);
  d += fmtRain('r', wxRain1hIn);
  d += fmtRain('p', wxRain24hIn);
  d += fmtHum(wxHum);
  d += fmtPressure(wxPresMb);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int y = 14;
  for (int i = 0; i < (int)d.length() && y <= 38; i += 21) {
    display.setCursor(0, y);
    display.println(d.substring(i, min((int)d.length(), i + 21)));
    y += 9;
  }

  display.drawFastHLine(0, 46, SCREEN_WIDTH, SSD1306_WHITE);
  drawRow("UTC", zuluTimestamp(), 50);
  drawRow("Age", String((millis() - lastWxSuccess) / 60000) + "m", 58);
}

void handleOled()
{
  if (!oledOk) return;

  unsigned long now = millis();
  if (lastPageTime != 0 && (now - lastPageTime < OLED_PAGE_MS)) return;
  lastPageTime = now;

  display.clearDisplay();
  switch (oledPage) {
    case 0: oledPageStatus();  break;
    case 1: oledPageWeather(); break;
    case 2: oledPagePacket();  break;
  }

  // invert ทั้งจอเฉพาะหน้า WX ตอนข้อมูลเก่า
  static bool inv = false;
  bool wantInv = (oledPage == 1 && wxValid && wxIsStale());
  if (wantInv != inv) {
    inv = wantInv;
    display.invertDisplay(inv);
  }

  display.display();
  oledPage = (oledPage + 1) % 3;
}


// =====================
// Setup
// =====================
void setup()
{
  Serial.begin(115200);
  delay(800);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  randomSeed(esp_random());
  blinkInterval = random(LED_MIN_MS, LED_MAX_MS + 1);

  oledInit();

  Serial.println();
  Serial.println("ESP32 APRS WX Beacon v2.3 (Lak Si) Starting");

  connectWiFi();

  configTime(0, 0, NTP_SERVER1, NTP_SERVER2);
  Serial.print("Sync NTP");
  for (int i = 0; i < 20 && time(nullptr) < 1600000000; i++) {
    delay(500); Serial.print(".");
  }
  Serial.println();

  fetchWeather();
  printWeather();
  lastWeatherTime = millis();

  sendAprsBeacon();
  lastBeaconTime = millis();
}


// =====================
// Loop
// =====================
void loop()
{
  handleLed();
  handleOled();

  unsigned long now = millis();

  if (now - lastWeatherTime >= WEATHER_INTERVAL_MS) {
    if (fetchWeather()) printWeather();
    lastWeatherTime = millis();
  }

  if (now - lastBeaconTime >= BEACON_INTERVAL_MS) {
    sendAprsBeacon();
    lastBeaconTime = millis();
  }
}