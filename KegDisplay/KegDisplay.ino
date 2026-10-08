/*
  KegDisplay - Waveshare ESP32-C6-LCD-1.47
  -----------------------------------------
  1. Boots, shows a splash screen. Press BOOT during the splash to erase saved WiFi.
  2. Tries to join the saved WiFi network.
  3. If that fails (or nothing is saved) it starts an access point "KegDisplay-Setup".
     Join it from your phone, a setup page pops up (or browse to 192.168.4.1),
     pick your network, enter the password, and the board reboots.
  4. Once on WiFi it polls http://kegerator.local/api every 2 seconds and shows
     psi, kegF and airF on the screen.

  Libraries (Arduino IDE -> Library Manager):
    - "GFX Library for Arduino" by Moon On Our Nation   (the display driver)
    - "ArduinoJson" by Benoit Blanchon (v7)
  Board: "ESP32C6 Dev Module" (esp32 by Espressif, 3.x). Set "USB CDC On Boot: Enabled"
  so Serial.print shows up in the Serial Monitor.
  Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)" - gives
  each of the two OTA program slots room to grow.

  Over-the-air updates: once the board is on WiFi it appears in
  Tools -> Port as a network port "kegdisplay at <ip>". Pick it and Upload as
  usual; the IDE asks for OTA_PASSWORD.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

// One set of values from the kegerator API.
// This must be defined BEFORE any function: the Arduino IDE quietly adds
// declarations for all your functions just above the first one, and
// fetchReading(Reading &r) needs to know what a Reading is at that point.
struct Reading {
  float psi, kegF, airF;
};

// ---------------------------------------------------------------------------
// Hardware pins (from the Waveshare wiki for this board)
// ---------------------------------------------------------------------------
#define LCD_MOSI  6   // SPI data out to the display
#define LCD_SCLK  7   // SPI clock
#define LCD_CS   14   // chip select: LOW = "display, I'm talking to you"
#define LCD_DC   15   // data/command: LOW = next byte is a command, HIGH = pixel data
#define LCD_RST  21   // hardware reset of the display controller
#define LCD_BL   22   // backlight LED. The LCD itself makes no light - this does.
#define BOOT_BTN  9   // the BOOT button (LOW when pressed)

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
const char *KEG_HOST = "kegerator";        // mDNS name, without ".local"
const char *KEG_PATH = "/api";
const char *AP_NAME  = "KegDisplay-Setup"; // open setup network name
const uint32_t POLL_MS         = 2000;
const uint32_t WIFI_TIMEOUT_MS = 15000;

const char *OTA_HOSTNAME = "kegdisplay";     // shows up as kegdisplay.local
const char *OTA_PASSWORD = "kegota";         // CHANGE THIS - asked for on every OTA upload

// Screen orientation:
//   0 = portrait                 1 = landscape
//   2 = portrait, upside down    3 = landscape, upside down
// Which end the USB port ends up at for each one is easiest to just try.
const uint8_t ROTATION = 0;

// ---------------------------------------------------------------------------
// Display objects
//
// Arduino_GFX splits a display into two layers:
//   "bus"  - HOW bytes get to the chip (here: hardware SPI on our pins)
//   "gfx"  - WHAT chip it is (ST7789) and its geometry
//
// The ST7789 controller has RAM for 240x320 pixels, but this glass is only
// 172 pixels wide, wired to the middle of that RAM. (240-172)/2 = 34, so we
// tell the driver to skip 34 columns. Wrong offset = picture shifted with
// garbage along one edge.
//
// Rotation tells the controller which way to fill its memory, so (0,0) is
// always the top-left corner as YOU see it. Our code just asks gfx->width()
// and gfx->height() and lays things out to fit.
// "true" = IPS panel, which needs colour inversion turned on. If your colours
// ever look like a photo negative, flip that to false.
// ---------------------------------------------------------------------------
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, ROTATION, true /* IPS */,
                                      172, 320,   // native width, height
                                      34, 0,      // col/row offset, rotation 0
                                      34, 0);     // col/row offset, rotation 2

// The gauge is drawn into a "canvas": a 172x172 picture held in the ESP32's
// own RAM (172 * 172 * 2 bytes = 58 KB). Each frame we draw the whole gauge
// there - background, ticks, needle - and flush() then copies the finished
// picture to the screen at (0,0) in one go. The screen never shows a
// half-drawn frame, so there's no flicker even though we redraw everything.
// (0,0) works for both orientations: the gauge is the left square in
// landscape and the top square in portrait.
const int GAUGE_SIZE = 172;
Arduino_Canvas *gauge = new Arduino_Canvas(GAUGE_SIZE, GAUGE_SIZE, gfx, 0, 0);

// ---------------------------------------------------------------------------
// Colours are RGB565: 16 bits per pixel = 5 bits red, 6 green, 5 blue.
//   RRRRRGGG GGGBBBBB
// This helper converts normal 0-255 RGB into that format at compile time.
// ---------------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t C_BG     = rgb565(0, 0, 0);
constexpr uint16_t C_TEXT   = rgb565(255, 255, 255);
constexpr uint16_t C_DIM    = rgb565(110, 110, 110);
constexpr uint16_t C_TITLE  = rgb565(40, 40, 60);
constexpr uint16_t C_AMBER  = rgb565(255, 170, 0);
constexpr uint16_t C_COLD   = rgb565(80, 180, 255);
constexpr uint16_t C_WARM   = rgb565(255, 70, 50);
constexpr uint16_t C_GOOD   = rgb565(60, 220, 90);

// Screen layout. These are filled in by setupLayout() once we know the
// orientation:
//   landscape (320 x 172): gauge square on the left, temps on the right
//   portrait  (172 x 320): gauge square on top, temps underneath
int screenW, screenH;
int colX;         // left edge of the temperature column
int rowY[2];      // top of the beer / air numbers (labels sit 20 px above)
int valueRight;   // numbers are right-aligned to this x, the degree-F goes after it
int statusY;      // top of the two status lines

// Gauge settings
const float GAUGE_MAX_PSI   = 30;
const float GAUGE_START_DEG = 150;   // where 0 psi sits (lower left)
const float GAUGE_SWEEP_DEG = 240;   // how far round the dial goes
// Coloured bands on the dial. Example values - set them to suit your setup.
const float ZONE_GOOD_LOW  = 10;     // green band from...
const float ZONE_GOOD_HIGH = 14;     // ...to
const float ZONE_HIGH      = 25;     // red from here up

// The built-in font is 6x8 pixels per character; setTextSize(n) scales it n times.
const int FONT_W = 6;
const int FONT_H = 8;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Preferences prefs;        // key/value storage in flash - survives reboot
WebServer   server(80);   // only used in setup (AP) mode
DNSServer   dns;          // only used in setup mode, makes the captive popup appear
bool        portalMode = false;
IPAddress   kegIP;        // cached result of the mDNS lookup
bool        kegResolved = false;

// What is currently drawn in each temperature row, so we only redraw on change.
String   shownText[2];
uint16_t shownColor[2];

// Gauge state
float targetPsi  = 0;     // latest reading from the API
float needlePsi  = 0;     // where the needle is drawn right now (catches up to targetPsi)
bool  psiStale   = true;  // true = no fresh data, draw the needle grey
bool  gaugeDirty = true;  // true = something changed, redraw even if the needle hasn't moved

// ===========================================================================
// Drawing helpers
// ===========================================================================

// Work out where things go for the current orientation.
void setupLayout() {
  screenW = gfx->width();    // already swapped for the rotation by the library
  screenH = gfx->height();

  if (screenW > screenH) {   // landscape
    colX       = GAUGE_SIZE + 8;
    rowY[0]    = 26;
    rowY[1]    = 94;
    valueRight = screenW - 30;
    statusY    = 144;
  } else {                   // portrait: everything starts below the gauge
    colX       = 6;
    rowY[0]    = GAUGE_SIZE + 22;
    rowY[1]    = GAUGE_SIZE + 80;
    valueRight = screenW - 32;
    statusY    = GAUGE_SIZE + 124;
  }
}

// Print text centred horizontally at row y. If it's too wide for the screen
// (common in portrait, only 172 px = 14 characters at size 2), use a smaller
// text size instead of running off the edge.
void printCentered(const String &s, int y, uint8_t size, uint16_t color) {
  while (size > 1 && (int)s.length() * FONT_W * size > screenW) size--;
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  int w = s.length() * FONT_W * size;
  gfx->setCursor((screenW - w) / 2, y);
  gfx->print(s);
}

void drawTitleBar(const char *title) {
  gfx->fillRect(0, 0, screenW, 26, C_TITLE);
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(8, 6);
  gfx->print(title);
}

// Two lines of tiny status text at the bottom of the temperature column
// (about 23 characters fit per line in landscape, 27 in portrait).
void drawStatus(const String &line1, const String &line2, uint16_t color) {
  gfx->fillRect(colX, statusY - 4, screenW - colX, 28, C_BG);
  gfx->setTextSize(1);
  gfx->setTextColor(color);
  gfx->setCursor(colX, statusY);
  gfx->print(line1);
  gfx->setCursor(colX, statusY + 12);
  gfx->print(line2);
}

// Draw the parts of the main screen that never change: labels and units.
void drawMainScreenStatic() {
  gfx->fillScreen(C_BG);

  const char *labels[2] = {"BEER", "AIR"};
  for (int i = 0; i < 2; i++) {
    gfx->setTextSize(2);
    gfx->setTextColor(C_DIM);
    gfx->setCursor(colX, rowY[i] - 20);
    gfx->print(labels[i]);

    // Units after the value. The default font has no degree sign handy,
    // so we draw a tiny circle ourselves - it's all just pixels.
    int ux = valueRight + 4;
    gfx->drawCircle(ux + 3, rowY[i] + 4, 3, C_DIM);
    gfx->setCursor(ux + 9, rowY[i]);
    gfx->print("F");

    shownText[i] = "";   // force the value to be drawn next time
  }
  gaugeDirty = true;     // and the gauge too
}

// Draw one big right-aligned number, but only if it changed.
//
// Why only on change? The display remembers every pixel by itself (it has its
// own RAM). We never "refresh" it - we only overwrite the pixels that need to
// change. Erasing + redrawing every time would cause visible flicker.
void drawValue(int row, const String &text, uint16_t color) {
  if (text == shownText[row] && color == shownColor[row]) return;

  const uint8_t size = 4;               // 24 x 32 pixel characters
  int w = text.length() * FONT_W * size;

  // Erase the old number: paint the value area with the background colour.
  gfx->fillRect(colX, rowY[row], valueRight - colX, FONT_H * size, C_BG);

  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(valueRight - w, rowY[row]);
  gfx->print(text);

  shownText[row]  = text;
  shownColor[row] = color;
}

// ===========================================================================
// The gauge
//
// Angles: 0 degrees points right (3 o'clock). Because screen y grows
// DOWNWARD, angles increase clockwise: 90 is straight down, 150 is lower-left
// (our 0 psi), 270 is straight up (15 psi) and 390 = 30 is lower-right (30 psi).
//
// Everything here draws on "gauge" (the canvas), not "gfx" (the screen).
// Same drawing functions - just a different target.
// ===========================================================================

const int GC = GAUGE_SIZE / 2;   // centre of the dial (x and y) inside the canvas

float psiToAngle(float psi) {
  psi = constrain(psi, 0, GAUGE_MAX_PSI);
  return GAUGE_START_DEG + psi / GAUGE_MAX_PSI * GAUGE_SWEEP_DEG;
}

// The core trick of every round gauge: the point at distance r from the
// centre, at angle deg, is (cos(angle) * r, sin(angle) * r).
void polar(float deg, float r, int &x, int &y) {
  float rad = deg * DEG_TO_RAD;
  x = GC + lroundf(cosf(rad) * r);
  y = GC + lroundf(sinf(rad) * r);
}

// A thick curved band from psi a to psi b, between radius rIn and rOut.
// Built from thin 2-degree slices, each slice drawn as two triangles.
void drawBand(float a, float b, int rIn, int rOut, uint16_t color) {
  float a0 = psiToAngle(a), a1 = psiToAngle(b);
  for (float d = a0; d < a1; d += 2) {
    float d2 = fminf(d + 2, a1);
    int x1, y1, x2, y2, x3, y3, x4, y4;
    polar(d,  rIn,  x1, y1);
    polar(d,  rOut, x2, y2);
    polar(d2, rIn,  x3, y3);
    polar(d2, rOut, x4, y4);
    gauge->fillTriangle(x1, y1, x2, y2, x3, y3, color);
    gauge->fillTriangle(x2, y2, x4, y4, x3, y3, color);
  }
}

void drawGauge() {
  gauge->fillScreen(C_BG);

  // Coloured zones around the outside
  drawBand(0,             GAUGE_MAX_PSI,  77, 80, C_DIM);
  drawBand(ZONE_GOOD_LOW, ZONE_GOOD_HIGH, 73, 80, C_GOOD);
  drawBand(ZONE_HIGH,     GAUGE_MAX_PSI,  73, 80, C_WARM);

  // Tick marks: short every 1 psi, long with a number every 5 psi
  gauge->setTextSize(1);
  gauge->setTextColor(C_TEXT);
  for (int p = 0; p <= GAUGE_MAX_PSI; p++) {
    bool major = (p % 5 == 0);
    float a = psiToAngle(p);
    int x1, y1, x2, y2;
    polar(a, major ? 60 : 65, x1, y1);
    polar(a, 70, x2, y2);
    gauge->drawLine(x1, y1, x2, y2, major ? C_TEXT : C_DIM);
    if (major) {
      int tx, ty;
      polar(a, 51, tx, ty);
      String s(p);
      gauge->setCursor(tx - s.length() * 3, ty - 4);  // centre the label on that point
      gauge->print(s);
    }
  }

  uint16_t color = psiStale ? C_DIM : C_AMBER;

  // Digital readout in the open space at the bottom of the dial.
  // Shows the real value, even if it's off the end of the scale.
  String txt = String(targetPsi, 1);
  gauge->setTextSize(3);
  gauge->setTextColor(psiStale ? C_DIM : C_TEXT);
  gauge->setCursor(GC - txt.length() * 9, GC + 30);
  gauge->print(txt);
  gauge->setTextSize(1);
  gauge->setTextColor(C_DIM);
  gauge->setCursor(GC - 9, GC + 58);
  gauge->print("PSI");

  // The needle: a long thin triangle. Its tip is out at the scale; its base is
  // two points 5 px either side of the centre, at right angles (+/- 90 deg)
  // to the direction the needle points.
  float a = psiToAngle(needlePsi);
  int tipX, tipY, lx, ly, rx, ry;
  polar(a,      66, tipX, tipY);
  polar(a - 90,  5, lx, ly);
  polar(a + 90,  5, rx, ry);
  gauge->fillTriangle(tipX, tipY, lx, ly, rx, ry, color);
  gauge->fillCircle(GC, GC, 8, color);   // hub
  gauge->fillCircle(GC, GC, 3, C_BG);

  gauge->flush();  // send the finished picture to the screen
}

// Called on every pass through loop(). About 30 times a second it moves the
// needle part of the way toward the real value. This is "easing": each frame
// covers 25% of the remaining distance, so the needle moves quickly at first
// and settles gently, like a real gauge.
void animateGauge() {
  static uint32_t lastFrame = 0;
  if (millis() - lastFrame < 33) return;
  lastFrame = millis();

  float diff = targetPsi - needlePsi;
  if (fabsf(diff) < 0.02f) {
    if (!gaugeDirty) return;   // nothing moved, nothing changed: skip the frame
    needlePsi = targetPsi;
  } else {
    needlePsi += diff * 0.25f;
  }
  drawGauge();
  gaugeDirty = false;
}

// ===========================================================================
// WiFi: normal (station) mode
// ===========================================================================

bool connectWiFi(const String &ssid, const String &pass) {
  gfx->fillScreen(C_BG);
  drawTitleBar("WIFI");
  printCentered("Connecting to", 50, 2, C_DIM);
  printCentered(ssid, 76, 2, C_TEXT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  int dots = 0;
  int maxDots = (screenW - 40) / 10;   // as many as fit across the screen
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    // simple progress bar so you can see it is alive
    gfx->fillRect(20 + (dots % maxDots) * 10, 120, 8, 8, C_AMBER);
    if (++dots % maxDots == 0) gfx->fillRect(20, 120, screenW - 40, 8, C_BG);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connect failed");
    WiFi.disconnect(true);
    return false;
  }

  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());
  setupOTA();  // also starts mDNS, which we need to look up kegerator.local
  return true;
}

// ===========================================================================
// Over-the-air updates
//
// The flash holds two program slots. While this program runs from one,
// ArduinoOTA receives the new program from the IDE and writes it into the
// other. Only when it has all arrived and checks out does the boot loader
// switch slots and reboot. A failed upload leaves the running program alone.
// ===========================================================================

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  // These "callbacks" are functions ArduinoOTA calls for us at each stage.
  // The [] () { ... } syntax is a lambda: a small unnamed function written inline.
  ArduinoOTA.onStart([]() {
    Serial.println("OTA start");
    gfx->fillScreen(C_BG);
    drawTitleBar("UPDATING");
    printCentered("Receiving firmware", 50, 2, C_DIM);
    gfx->drawRect(19, 89, screenW - 38, 22, C_DIM);  // progress bar outline
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    // Called many times per second during the upload - keep it quick.
    static int lastPct = -1;
    int pct = done * 100 / total;
    if (pct == lastPct) return;
    lastPct = pct;
    gfx->fillRect(20, 90, (screenW - 40) * pct / 100, 20, C_AMBER);
    gfx->fillRect(screenW / 2 - 30, 124, 60, 16, C_BG);
    printCentered(String(pct) + "%", 124, 2, C_TEXT);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("OTA done, rebooting");
    printCentered("Done - rebooting", 150, 1, C_GOOD);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error %u\n", error);
    gfx->fillRect(0, 140, screenW, 32, C_BG);
    printCentered(error == OTA_AUTH_ERROR ? "Wrong OTA password" : "Update failed",
                  146, 2, C_WARM);
    delay(2500);
    drawMainScreenStatic();  // back to normal; the old program is still running
  });

  ArduinoOTA.begin();
  Serial.printf("OTA ready: %s.local\n", OTA_HOSTNAME);
}

// ===========================================================================
// WiFi: setup portal (access point) mode
// ===========================================================================

String scannedOptions;  // <option> list of nearby networks for the web form

String htmlEscape(const String &s) {
  String out;
  for (char c : s) {
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void handleRoot() {
  String page =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
    "<title>KegDisplay setup</title>"
    "<style>body{font-family:sans-serif;max-width:360px;margin:24px auto;padding:0 12px}"
    "input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box;font-size:16px}"
    "button{padding:10px 20px;font-size:16px}</style></head><body>"
    "<h2>KegDisplay WiFi</h2>"
    "<form method=POST action=/save>"
    "Network<input name=ssid list=nets autocomplete=off required>"
    "<datalist id=nets>" + scannedOptions + "</datalist>"
    "Password<input name=pass type=password>"
    "<button type=submit>Save &amp; reboot</button>"
    "</form><p style='color:#888'>2.4 GHz networks only.</p></body></html>";
  server.send(200, "text/html", page);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  Serial.printf("Saved WiFi '%s', rebooting\n", ssid.c_str());

  server.send(200, "text/html",
              "<html><body style='font-family:sans-serif'><h2>Saved.</h2>"
              "Rebooting and joining <b>" + htmlEscape(ssid) + "</b>...</body></html>");
  delay(1500);
  ESP.restart();
}

// Phones probe random URLs to detect captive portals. Sending everything
// back to our page is what makes the "Sign in to network" popup appear.
void handleNotFound() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void startPortal() {
  portalMode = true;

  // Scan first, while the radio is free, so the form can suggest networks.
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    scannedOptions += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  }

  WiFi.softAP(AP_NAME);  // open network, no password
  dns.start(53, "*", WiFi.softAPIP());  // answer every DNS lookup with our IP

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  gfx->fillScreen(C_BG);
  drawTitleBar("WIFI SETUP");
  printCentered("On your phone, join:", 40, 2, C_DIM);
  printCentered(AP_NAME, 66, 2, C_AMBER);
  printCentered("then open", 102, 2, C_DIM);
  printCentered(WiFi.softAPIP().toString(), 128, 2, C_AMBER);

  Serial.printf("Portal up: join '%s', browse to %s\n",
                AP_NAME, WiFi.softAPIP().toString().c_str());
}

// ===========================================================================
// Kegerator API
// ===========================================================================

// Turn "kegerator" into an IP address using mDNS.
// (Plain DNS doesn't know about .local names, so we ask with mDNS directly.)
bool resolveKeg() {
  kegIP = MDNS.queryHost(KEG_HOST, 2000);
  kegResolved = !(kegIP == IPAddress(0, 0, 0, 0));
  if (!kegResolved) {
    Serial.println("mDNS lookup failed");
    return false;
  }
  Serial.print("kegerator.local = ");
  Serial.println(kegIP);
  return true;
}

// Fetch and parse {"psi":-4.54,"kegF":41.3,"airF":41.3}
bool fetchReading(Reading &r) {
  if (!kegResolved && !resolveKeg()) return false;

  HTTPClient http;
  String url = "http://" + kegIP.toString() + KEG_PATH;
  http.setTimeout(3000);
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("HTTP error %d\n", code);
    http.end();
    kegResolved = false;  // look the name up again next time, in case its IP changed
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return false;
  }

  // "| NAN" means: use NAN (not-a-number) if the key is missing
  r.psi  = doc["psi"]  | NAN;
  r.kegF = doc["kegF"] | NAN;
  r.airF = doc["airF"] | NAN;

  // The pressure sensor still reads slightly below zero at times; a keg can't
  // really be under vacuum, so treat anything negative as 0.
  if (r.psi < 0) r.psi = 0;
  return !isnan(r.psi) && !isnan(r.kegF) && !isnan(r.airF);
}

uint16_t tempColor(float f) {
  if (f <= 38) return C_COLD;
  if (f <= 45) return C_GOOD;
  return C_WARM;
}

// ===========================================================================
// setup / loop
// ===========================================================================

void setup() {
  Serial.begin(115200);

  // Display on
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);  // try analogWrite(LCD_BL, 0-255) to dim it
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  // Allocate the gauge canvas's 58 KB of RAM. GFX_SKIP_OUTPUT_BEGIN = don't
  // initialise the screen again, gfx->begin() already did that.
  if (!gauge->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("gauge canvas: not enough memory!");
  }
  setupLayout();
  gfx->fillScreen(C_BG);

  // Splash + chance to wipe WiFi settings.
  // (BOOT can't be held during reset - GPIO9 is a strapping pin and holding it
  // puts the chip into firmware-download mode - so we check it after boot.)
  pinMode(BOOT_BTN, INPUT_PULLUP);
  drawTitleBar("KEG DISPLAY");
  printCentered("Press BOOT now to", 60, 2, C_DIM);
  printCentered("reset WiFi", 84, 2, C_DIM);

  prefs.begin("wifi", false);  // "wifi" namespace, read/write

  bool wipe = false;
  for (int i = 0; i < 20 && !wipe; i++) {  // 2 second window
    gfx->fillRect(20, 130, (screenW - 40) * (i + 1) / 20, 6, C_AMBER);
    if (digitalRead(BOOT_BTN) == LOW) wipe = true;
    delay(100);
  }
  if (wipe) {
    prefs.clear();
    gfx->fillScreen(C_BG);
    printCentered("WiFi settings erased", 80, 2, C_WARM);
    delay(1500);
  }

  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.isEmpty() || !connectWiFi(ssid, pass)) {
    startPortal();
    return;
  }

  drawMainScreenStatic();
  drawStatus("Looking for", String(KEG_HOST) + ".local...", C_DIM);
}

void loop() {
  if (portalMode) {
    dns.processNextRequest();
    server.handleClient();
    return;
  }

  // Must run often: this is where the board notices the IDE wants to upload.
  ArduinoOTA.handle();
  animateGauge();

  static uint32_t lastPoll = 0;
  static uint32_t lastGood = 0;
  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();

  if (WiFi.status() != WL_CONNECTED) {
    // The ESP32 reconnects automatically; we just report it.
    drawStatus("WiFi lost,", "reconnecting...", C_WARM);
    return;
  }

  Reading r;
  if (fetchReading(r)) {
    lastGood = millis();
    if (r.psi != targetPsi || psiStale) gaugeDirty = true;
    targetPsi = r.psi;     // animateGauge() swings the needle over to it
    psiStale  = false;
    drawValue(0, String(r.kegF, 1), tempColor(r.kegF));
    drawValue(1, String(r.airF, 1), tempColor(r.airF));
    drawStatus("me   " + WiFi.localIP().toString(), "keg  " + kegIP.toString(), C_DIM);
  } else {
    // Keep the last values but grey them out so stale data is obvious.
    if (!psiStale) gaugeDirty = true;
    psiStale = true;
    for (int i = 0; i < 2; i++) {
      if (shownText[i].length()) drawValue(i, shownText[i], C_DIM);
    }
    String age = lastGood ? String((millis() - lastGood) / 1000) + "s ago" : "never";
    drawStatus("No data from keg", "last: " + age, C_WARM);
  }
}
