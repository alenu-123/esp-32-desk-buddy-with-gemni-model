/*
  ESP32 OLED - RoboEyes / NTP Clock / Gemini Live Transcript
  ---------------------------------------------------------------------------
  Press the BOOT button (GPIO0) to cycle:
    EYES -> CLOCK -> TRANSCRIPT -> EYES -> ...

  EYES mode: full-screen RoboEyes animation (blink, idle look-around).
  CLOCK mode: big NTP-synced time + date.
  TRANSCRIPT mode: full-screen scrolling transcript text from Gemini Live
    (POST to /text). No eyes drawn here - eliminates the flicker caused by
    redrawing animated eyes every frame underneath text.

  Wiring (standard I2C):
    OLED SDA -> GPIO21
    OLED SCL -> GPIO22
    OLED VCC -> 3V3
    OLED GND -> GND
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

// ---------- Display config ----------
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDR    0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// RoboEyes instance driving the same display
RoboEyes<Adafruit_SSD1306> roboEyes(display);

// ---------- Button config ----------
#define BOOT_PIN 0
unsigned long lastPressTime = 0;
const unsigned long debounceMs = 250;
bool lastButtonState = HIGH;

// ---------- WiFi / NTP config ----------
const char* ssid     = "moto g35 5G_7938";
const char* password = "butterfly";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffsetSec = 5 * 3600 + 1800; // example: IST (+5:30). Change for your timezone.
const int   daylightOffsetSec = 0;
bool timeSynced = false;

WebServer server(80);

// ---------- Menu state ----------
enum Mode { EYES_ONLY, CLOCK, TRANSCRIPT, MODE_COUNT };
Mode currentMode = EYES_ONLY;

// ---------- Layout (transcript screen, full-screen now) ----------
const int textAreaTop     = 0;
const int charWidth       = 6;
const int lineHeight       = 8;
const int maxCharsPerLine = SCREEN_WIDTH / charWidth;
const int maxVisibleLines = (SCREEN_HEIGHT - textAreaTop) / lineHeight;

// ---------- Line buffer ----------
#define MAX_LINES 40
String lineBuffer[MAX_LINES];
int lineCount = 0;
bool transcriptDirty = true; // forces one draw when we enter the mode / new line arrives

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDR)) {
    Serial.println("SSD1306 allocation failed");
    for (;;) delay(10);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.display();

  connectWiFiAndSyncTime();

  // Full-screen centered eyes (no transcript sharing the screen anymore)
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 60); // width, height, max framerate
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);

  server.on("/text", HTTP_POST, handleText);
  server.begin();
  Serial.println("HTTP server started");

  addLine("Ready. Waiting for");
  addLine("transcript...");
}

void loop() {
  server.handleClient();
  handleButton();

  switch (currentMode) {
    case EYES_ONLY:
      roboEyes.update(); // full-screen eyes, nothing else drawn
      break;
    case CLOCK:
      drawClock();
      break;
    case TRANSCRIPT:
      drawTranscript();
      break;
    default:
      break;
  }
}

// ---------------- Button handling ----------------
void handleButton() {
  bool reading = digitalRead(BOOT_PIN);

  if (lastButtonState == HIGH && reading == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime > debounceMs) {
      lastPressTime = now;
      currentMode = static_cast<Mode>((currentMode + 1) % MODE_COUNT);
      display.clearDisplay();
      display.display();
      if (currentMode == TRANSCRIPT) transcriptDirty = true; // force redraw on entry
      Serial.print("Mode switched to: ");
      if (currentMode == EYES_ONLY) Serial.println("EYES_ONLY");
      else if (currentMode == CLOCK) Serial.println("CLOCK");
      else Serial.println("TRANSCRIPT");
    }
  }
  lastButtonState = reading;
}

// ---------------- WiFi + NTP ----------------
void connectWiFiAndSyncTime() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.begin(ssid, password);
  WiFi.setSleep(false); // reduces HTTP response latency for /text posts
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.println(WiFi.localIP());

    configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      timeSynced = true;
      Serial.println("Time synced.");
    } else {
      Serial.println("Failed to obtain time.");
    }

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi connected");
    display.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connect failed");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi FAILED");
  }
  display.display();
  delay(1500);
  display.clearDisplay();
  display.display();
}

// ---------------- HTTP handler ----------------
void handleText() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No body");
    return;
  }
  String incoming = server.arg("plain");
  incoming.trim();
  if (incoming.length() == 0) {
    server.send(200, "text/plain", "empty ignored");
    return;
  }

  wrapAndAdd(incoming);
  server.send(200, "text/plain", "ok");
}

// ---------------- Word wrap ----------------
void wrapAndAdd(String text) {
  while (text.length() > 0) {
    if ((int)text.length() <= maxCharsPerLine) {
      addLine(text);
      break;
    }
    int breakAt = maxCharsPerLine;
    int lastSpace = text.lastIndexOf(' ', maxCharsPerLine);
    if (lastSpace > 0) breakAt = lastSpace;

    addLine(text.substring(0, breakAt));
    text = text.substring(breakAt);
    text.trim();
  }
}

// ---------------- Line buffer management ----------------
void addLine(String line) {
  if (lineCount < MAX_LINES) {
    lineBuffer[lineCount++] = line;
  } else {
    for (int i = 1; i < MAX_LINES; i++) {
      lineBuffer[i - 1] = lineBuffer[i];
    }
    lineBuffer[MAX_LINES - 1] = line;
  }
  transcriptDirty = true; // new content -> redraw next time we're in TRANSCRIPT mode
}

// ---------------- Transcript screen (full-screen, only redraws on change) ----------------
void drawTranscript() {
  if (!transcriptDirty) return; // nothing new - skip the redraw, this is what kills the flicker
  transcriptDirty = false;

  display.clearDisplay();

  int start = max(0, lineCount - maxVisibleLines);
  for (int i = start; i < lineCount; i++) {
    display.setCursor(0, textAreaTop + (i - start) * lineHeight);
    display.println(lineBuffer[i]);
  }

  display.display();
}

// ---------------- Clock screen ----------------
void drawClock() {
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();
  if (now - lastUpdate < 500) return;
  lastUpdate = now;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  struct tm timeinfo;
  if (timeSynced && getLocalTime(&timeinfo, 100)) {
    char timeStr[9];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    char dateStr[16];
    strftime(dateStr, sizeof(dateStr), "%d %b %Y", &timeinfo);

    display.setTextSize(2);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, SCREEN_HEIGHT / 2 - 16);
    display.println(timeStr);

    display.setTextSize(1);
    display.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, SCREEN_HEIGHT / 2 + 12);
    display.println(dateStr);
  } else {
    display.setTextSize(1);
    display.setCursor(10, SCREEN_HEIGHT / 2 - 4);
    display.println("No time - check WiFi");
  }

  display.display();
}
