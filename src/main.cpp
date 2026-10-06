// Jarvis: a pocket voice search for the M5StickC Plus2.
//
// Hold the M5 button and ask a question out loud. On release the audio goes
// to Whisper on Groq for speech-to-text, Tavily searches the web, and GPT-OSS
// on Groq answers from the results. Everything runs on free plans.
//
// Held with the M5 button on the right, the side button is on top and the
// power button at the bottom. M5: hold to ask, click for the home screen. Top: click scrolls up, hold
// 1 s opens the Wi-Fi menu. Bottom: click scrolls down, hold 1 s shows the info
// screen, about 2.2 s powers off. While a question is being answered, three
// quick M5 taps cancel it.
//
// The UI runs in loop() at about 30 fps. Network requests run in a separate
// task so the animations keep moving while it waits.

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <Preferences.h>
#include <algorithm>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <atomic>
#include <time.h>
#include <vector>

#include "certs.h"
#include "secrets.h"
#include "avatar.h"

#ifndef WIFI_OPEN
#define WIFI_OPEN nullptr
#endif

#ifndef TAVILY_API_KEY
#define TAVILY_API_KEY "tvly-..."
#endif

#ifndef TIMEZONE
#define TIMEZONE "UTC0"
#endif

// ---- Audio ----
static const uint32_t SAMPLE_RATE = 16000;
static const size_t MAX_SAMPLES = SAMPLE_RATE * 15;     // 15 s cap
static const size_t MIN_SAMPLES = SAMPLE_RATE * 2 / 5;  // ignore taps under 0.4 s
static const size_t CHUNK = 800;                        // 50 ms per mic request
static const size_t WAV_HEADER = 44;
static const size_t PRE_ROOM = 512;  // multipart preamble goes before the WAV
static const size_t POST_ROOM = 64;  // and the closing boundary after it

static const char *BOUNDARY = "----jarvis7d1f";


// One PSRAM buffer laid out as [preamble room][WAV header][PCM][closing boundary]
// so the upload body is built without copying the audio.
static uint8_t *uploadBuf;
static int16_t *pcm;

// ---- Look ----
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t BG = rgb(10, 12, 18);
static const uint16_t SURFACE = rgb(30, 34, 46);
static const uint16_t TEXT = rgb(236, 238, 242);
static const uint16_t MUTED = rgb(140, 148, 166);
static const uint16_t ACCENT = rgb(255, 138, 61);
static const uint16_t DANGER = rgb(255, 92, 92);

static const lgfx::IFont *FONT_SMALL = &fonts::DejaVu12;
static const lgfx::IFont *FONT_BODY = &fonts::FreeSans9pt7b;
static const lgfx::IFont *FONT_TITLE = &fonts::FreeSansBold12pt7b;
static const lgfx::IFont *FONT_SUBTITLE = &fonts::FreeSansBold9pt7b;

static const int W = 240, H = 135, BAR_H = 16;

static M5Canvas canvas(&M5.Display);

// ---- State shared with the network task ----
enum Stage { ST_IDLE, ST_TRANSCRIBE, ST_SEARCH, ST_THINK, ST_DONE, ST_ERROR };
static std::atomic<int> stage{ST_IDLE};
// Written by the network task before it publishes the stage that exposes them.
enum Job { JOB_CONNECT, JOB_JOIN, JOB_ASK };
// Each question gets a number. Cancelling bumps it, so a question still in
// flight sees it's been cancelled and drops its results.
static std::atomic<uint32_t> jobSeq{0};
static uint32_t runningJob = 0;  // network task only
static std::atomic<int> job{JOB_CONNECT};
static std::atomic<bool> netBusy{false};
static std::atomic<int> connectResult{0};  // 0 pending, 1 connected, 2 failed
static std::atomic<bool> cancelConnect{false};  // the Wi-Fi menu wants the radio
static String joinSsid, joinPass;
static size_t jobSamples;
static String jobQuestion, jobAnswer, jobErrorTitle, jobError;
static bool jobSearched;
static TaskHandle_t netTask;

// ---- UI state ----
enum class Screen { Connecting, Home, Listening, Working, Answer, Error, Wifi, Info, Bye };
static Screen screen = Screen::Connecting;
static Screen screenBeforeListening = Screen::Home;
static uint32_t screenSince = 0;
static String errorTitle, errorText;
static bool errorIsWifi = false;  // Wi-Fi errors offer the network list
static String connectingLabel = "Looking for Wi-Fi";
static String toast;
static uint32_t toastUntil = 0;
static uint32_t lastActivity = 0;
// The screen dims after 30 s idle and switches off after 2 min. The press
// that wakes it is swallowed, so it can't also start a recording or count
// toward the power-off hold.
enum class Backlight { On, Dim, Off };
static Backlight backlight = Backlight::On;
static bool swallowPress = false;
static const uint8_t BRIGHT = 160, DIM = 48;
static bool haveAnswer = false;
static bool answerSearched = true;

// Recording
static size_t recSamples = 0;
static float levels[22];
static float level = 0;
static uint32_t lastLevelPush = 0;
static bool sampleListening = false;  // "show listen" holds a fake recording on screen

// Answer layout
struct TextLine {
  String text;
  const lgfx::IFont *font;
  uint16_t color;
  int y;
};
static std::vector<TextLine> layout;
static int questionBoxH = 0, contentH = 0;
static float scrollPos = 0;
static int scrollTarget = 0;

// Wi-Fi menu
enum Access { ACCESS_NONE, ACCESS_SAVED, ACCESS_OPEN };
struct ScanEntry {
  String ssid;
  int rssi;
  bool open;      // the network has no password
  Access access;  // what secrets.h says about it
};
static std::vector<ScanEntry> scanList;
static int wifiSel = 0, wifiTop = 0;
static bool scanning = false, scanPending = false;
static uint32_t lastScan = 0;
static Screen screenBeforeWifi = Screen::Home;

// Info screen
static Screen infoReturn = Screen::Home;
static uint32_t infoUntil = 0;
// The bottom (power) button opens the info screen after 1 s. The Plus2's power
// circuit cuts power when that button is held for about 2.5 s (measured; the
// label says 6 s), so Jarvis shuts down cleanly just before that.
static const uint32_t INFO_MS = 5000, INFO_HOLD_MS = 1000, POWER_OFF_MS = 2200;
static const uint32_t TOP_HOLD_MS = 1000;  // top button: Wi-Fi menu
static Preferences prefs;

// Status bar
static int batteryPct = -1;
static uint32_t lastBatteryRead = 0;

// One of these greets you on the home screen, a new one each visit.
static const char *TAGLINES[] = {
  "Ask me anything.",
  "Smarter than I look.",
  "Knows stuff. Mostly.",
  "Pocket-sized genius.",
  "Try me. I dare you.",
  "I google so you don't.",
  "Small stick, big brain.",
  "No question too weird.",
};
static int tagline = 0;

static void setScreen(Screen s)
{
  if (s == Screen::Home && screen != Screen::Home)
    tagline = esp_random() % (sizeof(TAGLINES) / sizeof(TAGLINES[0]));
  screen = s;
  screenSince = millis();
}

static void showToast(const String &msg)
{
  toast = msg;
  toastUntil = millis() + 2200;
}

// ---- Text ----

// Folds text to the 7-bit ASCII the fonts can draw.
static String toAscii(const char *s)
{
  static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
  String out;
  bool skipping = false;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    uint32_t cp;
    int n;
    if (*p < 0x80) { cp = *p; n = 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 4; }
    else { p++; continue; }
    int i = 1;
    for (; i < n && (p[i] & 0xC0) == 0x80; i++) cp = (cp << 6) | (p[i] & 0x3F);
    p += i;
    if (i < n) continue;  // truncated sequence

    if (cp == 0x3010) {  // skip a 【citation】 marker
      skipping = true;
      continue;
    }
    if (skipping) {
      if (cp == 0x3011) skipping = false;
      continue;
    }

    if (cp == '\n' || (cp >= 0x20 && cp < 0x7F)) out += (char)cp;
    else if (cp >= 0xC0 && cp <= 0xFF) out += latin1[cp - 0xC0];
    else switch (cp) {
      case 0x2018: case 0x2019: case 0x201B: case 0x2032: out += '\''; break;
      case 0x201C: case 0x201D: case 0x2033: out += '"'; break;
      case 0x2010: case 0x2011: case 0x2013: case 0x2014: case 0x2212: out += '-'; break;
      case 0x2022: case 0x00B7: out += '-'; break;
      case 0x2026: out += "..."; break;
      case 0x00A0: case 0x2009: case 0x202F: out += ' '; break;
      case 0x00B0: out += " deg"; break;
      case 0x00D7: out += 'x'; break;
      case 0x20AC: out += "EUR"; break;
      case 0x00A3: out += "GBP"; break;
      case 0x20B9: out += "Rs"; break;
      default: break;  // anything else is dropped
    }
  }
  out.replace("**", "");
  out.replace("`", "");
  out.trim();
  return out;
}

// Word-wraps `text` to `maxW` pixels in `font`, keeping paragraph breaks.
static std::vector<String> wrapText(const String &text, const lgfx::IFont *font, int maxW)
{
  std::vector<String> out;
  canvas.setFont(font);
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String para = text.substring(start, nl);
    start = nl + 1;

    String line;
    int i = 0;
    while (i < (int)para.length()) {
      int sp = para.indexOf(' ', i);
      if (sp < 0) sp = para.length();
      String word = para.substring(i, sp);
      i = sp + 1;
      if (word.isEmpty()) continue;
      String trial = line.isEmpty() ? word : line + " " + word;
      if (canvas.textWidth(trial) <= maxW) {
        line = trial;
        continue;
      }
      if (!line.isEmpty()) out.push_back(line);
      while (canvas.textWidth(word) > maxW) {  // split words wider than the line
        int cut = word.length() - 1;
        while (cut > 1 && canvas.textWidth(word.substring(0, cut)) > maxW) cut--;
        out.push_back(word.substring(0, cut));
        word = word.substring(cut);
      }
      line = word;
    }
    // Keep paragraph breaks, but never two blank lines in a row.
    if (!line.isEmpty() || (!out.empty() && !out.back().isEmpty())) out.push_back(line);
  }
  while (!out.empty() && out.back().isEmpty()) out.pop_back();
  return out;
}

// Wraps to at most `maxLines`, ending the last one with "..." if cut short.
static std::vector<String> wrapClamped(const String &text, const lgfx::IFont *font, int maxW, size_t maxLines)
{
  auto lines = wrapText(text, font, maxW);
  if (lines.size() > maxLines) {
    lines.resize(maxLines);
    String &last = lines.back();
    while (last.length() && canvas.textWidth(last + "...") > maxW) last.remove(last.length() - 1);
    last.trim();
    last += "...";
  }
  return lines;
}

// ---- Drawing helpers ----

static String fitText(String text, int maxW);

static void drawStatusBar()
{
  canvas.setFont(FONT_SMALL);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(MUTED);
  struct tm t;
  String left;  // the clock, once it has synced
  if (getLocalTime(&t, 0) && t.tm_year >= 125) {
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &t);
    left = buf;
  }
  if (screen != Screen::Connecting) canvas.drawString(left, 6, BAR_H / 2);
  if (screen == Screen::Answer && !answerSearched) {
    int x = 8 + canvas.textWidth(left) + 6;
    canvas.fillSmoothRoundRect(x, 2, 46, 12, 6, SURFACE);
    canvas.setTextDatum(middle_center);
    canvas.drawString("no web", x + 23, BAR_H / 2);
  }

  // Battery
  if (batteryPct < 0 || millis() - lastBatteryRead > 5000) {
    batteryPct = M5.Power.getBatteryLevel();
    lastBatteryRead = millis();
  }
  int bx = W - 26, by = 4;
  canvas.drawRoundRect(bx, by, 19, 9, 2, MUTED);
  canvas.fillRect(bx + 19, by + 3, 2, 3, MUTED);
  int fill = constrain(batteryPct, 0, 100) * 15 / 100;
  canvas.fillRect(bx + 2, by + 2, max(fill, 1), 5, batteryPct <= 20 ? DANGER : TEXT);

  // Wi-Fi bars
  int bars = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    bars = rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
  }
  for (int i = 0; i < 3; i++) {
    int h = 3 + i * 3;
    canvas.fillRect(W - 42 + i * 4, 13 - h, 3, h, i < bars ? TEXT : SURFACE);
  }
}

static void drawToast()
{
  if (millis() > toastUntil) return;
  canvas.setFont(FONT_SMALL);
  int w = canvas.textWidth(toast) + 20;
  int x = (W - w) / 2, y = H - 24;
  canvas.fillSmoothRoundRect(x, y, w, 18, 9, SURFACE);
  canvas.setTextColor(TEXT);
  canvas.setTextDatum(middle_center);
  canvas.drawString(toast, W / 2, y + 9);
}

static float seconds() { return millis() / 1000.0f; }

// ---- Screens ----

static void drawConnecting()
{
  drawAvatar(canvas, 0, 20, Mood::Sleepy);

  int x = 100;
  canvas.setTextDatum(top_left);
  canvas.setFont(FONT_SUBTITLE);
  canvas.setTextColor(TEXT);
  canvas.drawString("Waking up", x, 40);
  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(MUTED);
  auto lines = wrapClamped(connectingLabel, FONT_SMALL, W - x - 4, 2);
  for (size_t i = 0; i < lines.size(); i++) canvas.drawString(lines[i], x, 66 + i * 13);

  int cx = x + 8, cy = 108;
  float a = fmodf(seconds() * 360.0f, 360.0f);
  canvas.fillArc(cx, cy, 5, 7, 0, 360, SURFACE);
  canvas.fillArc(cx, cy, 5, 7, a, min(a + 100, 360.0f), ACCENT);
  if (a + 100 > 360) canvas.fillArc(cx, cy, 5, 7, 0, a + 100 - 360, ACCENT);
}

static void drawHome()
{
  drawAvatar(canvas, 0, 20, Mood::Idle);

  int x = 100;
  canvas.setTextDatum(top_left);
  canvas.setFont(FONT_BODY);
  canvas.setTextColor(MUTED);
  canvas.drawString("Hi, I'm", x, 22);
  canvas.setFont(FONT_TITLE);
  canvas.setTextColor(TEXT);
  canvas.drawString("JARVIS", x, 40);
  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(ACCENT);
  auto tag = wrapClamped(TAGLINES[tagline], FONT_SMALL, W - x - 4, 2);
  for (size_t i = 0; i < tag.size(); i++) canvas.drawString(tag[i], x, 66 + i * 13);

  canvas.setTextColor(MUTED);
  canvas.drawString("Hold M5 to ask me", x, 98);
  canvas.setTextColor(haveAnswer ? ACCENT : MUTED);
  canvas.drawString(haveAnswer ? "Tap M5: last answer" : "Hold top: Wi-Fi", x, 113);
}

static void drawListening()
{
  drawAvatar(canvas, 0, 20, Mood::Listening, level);

  int x = 100;
  canvas.setTextDatum(top_left);
  canvas.setFont(FONT_SUBTITLE);
  canvas.setTextColor(ACCENT);
  canvas.drawString("Listening", x, 24);

  size_t secs = recSamples / SAMPLE_RATE;
  char timer[8];
  snprintf(timer, sizeof(timer), "0:%02u", (unsigned)secs);
  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(MUTED);
  canvas.setTextDatum(top_right);
  canvas.drawString(timer, W - 6, 27);

  // Waveform, newest on the right
  const int n = sizeof(levels) / sizeof(levels[0]);
  int mid = 72;
  for (int i = 0; i < n; i++) {
    int h = 3 + (int)(levels[i] * 34);
    canvas.fillSmoothRoundRect(x + i * 6, mid - h / 2, 4, h, 2, i == n - 1 ? ACCENT : TEXT);
  }

  int pw = W - 6 - x;
  canvas.fillSmoothRoundRect(x, 100, pw, 4, 2, SURFACE);
  canvas.fillSmoothRoundRect(x, 100, max(4, (int)(pw * recSamples / MAX_SAMPLES)), 4, 2, ACCENT);
  canvas.setTextDatum(top_left);
  canvas.drawString("Let go to send", x, 112);
}

static void drawWorking()
{
  drawAvatar(canvas, 0, 20, Mood::Thinking);

  int st = stage.load(std::memory_order_acquire);
  const int x = 100, colW = W - x - 4, cx = x + colW / 2;
  int dotsY = 62;
  if (st >= ST_SEARCH) {
    auto lines = wrapClamped(toAscii(jobQuestion.c_str()), FONT_SMALL, colW - 14, 3);
    int boxH = lines.size() * 14 + 8;
    canvas.fillSmoothRoundRect(x, BAR_H + 4, colW, boxH, 7, SURFACE);
    canvas.setTextColor(MUTED);
    canvas.setTextDatum(top_left);
    for (size_t i = 0; i < lines.size(); i++) canvas.drawString(lines[i], x + 7, BAR_H + 9 + i * 14);
    dotsY = BAR_H + 4 + boxH + 18;
  }

  float t = seconds();
  for (int i = 0; i < 3; i++) {
    float bounce = fabsf(sinf(t * 5.0f - i * 0.8f));
    canvas.fillSmoothCircle(cx - 14 + i * 14, dotsY - (int)(bounce * 6), 4, i == 1 ? TEXT : ACCENT);
  }

  const char *label = st == ST_TRANSCRIBE ? "Listening back"
                    : st == ST_SEARCH ? "Searching"
                    : "Thinking";
  canvas.setFont(FONT_BODY);
  canvas.setTextColor(TEXT);
  canvas.setTextDatum(top_center);
  canvas.drawString(fitText(label, colW), cx, dotsY + 10);

  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(MUTED);
  canvas.setTextDatum(bottom_center);
  canvas.drawString("Tap M5 3x to cancel", cx, H - 2);
}

static const int VIEW_TOP = BAR_H + 2;
static const int VIEW_H = H - VIEW_TOP;

static int maxScroll() { return max(0, contentH - VIEW_H); }

static void layoutAnswer(const String &question, const String &answer)
{
  layout.clear();
  int y = 4;
  for (auto &s : wrapText(toAscii(question.c_str()), FONT_SMALL, W - 30)) {
    layout.push_back({s, FONT_SMALL, MUTED, y + 5});
    y += 15;
  }
  questionBoxH = y + 8;
  y = questionBoxH + 10;
  for (auto &s : wrapText(toAscii(answer.c_str()), FONT_BODY, W - 22)) {
    layout.push_back({s, FONT_BODY, TEXT, y});
    y += 19;
  }
  contentH = y + 4;
  scrollPos = 0;
  scrollTarget = 0;
}

static void drawAnswer()
{
  scrollPos += (scrollTarget - scrollPos) * 0.35f;
  if (fabsf(scrollTarget - scrollPos) < 0.5f) scrollPos = scrollTarget;
  int off = VIEW_TOP - (int)scrollPos;

  canvas.setClipRect(0, VIEW_TOP, W, VIEW_H);
  canvas.fillSmoothRoundRect(6, off + 4, W - 16, questionBoxH - 4, 7, SURFACE);
  canvas.setTextDatum(top_left);
  for (auto &l : layout) {
    int y = off + l.y;
    if (y > H || y < VIEW_TOP - 24) continue;
    canvas.setFont(l.font);
    canvas.setTextColor(l.color);
    canvas.drawString(l.text, l.font == FONT_SMALL ? 14 : 8, y);
  }
  canvas.clearClipRect();

  if (contentH > VIEW_H) {
    int thumbH = max(12, VIEW_H * VIEW_H / contentH);
    int thumbY = VIEW_TOP + (int)((VIEW_H - thumbH) * scrollPos / maxScroll());
    canvas.fillRect(W - 3, VIEW_TOP, 2, VIEW_H, SURFACE);
    canvas.fillRect(W - 3, thumbY, 2, thumbH, ACCENT);
  }
}

static void drawError()
{
  drawAvatar(canvas, 0, 20, Mood::Error);

  int x = 100, colW = W - x - 4;
  canvas.setTextDatum(top_left);
  canvas.setFont(FONT_SUBTITLE);
  canvas.setTextColor(TEXT);
  auto title = wrapClamped(errorTitle, FONT_SUBTITLE, colW, 2);
  int y = 24;
  for (auto &l : title) {
    canvas.drawString(l, x, y);
    y += 18;
  }
  y += 4;

  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(MUTED);
  int bottom = errorIsWifi ? 103 : 117;
  auto lines = wrapClamped(errorText, FONT_SMALL, colW, max(1, (bottom - y) / 13));
  for (size_t i = 0; i < lines.size(); i++) canvas.drawString(lines[i], x, y + i * 13);

  canvas.setTextColor(ACCENT);
  if (errorIsWifi) {
    canvas.drawString("Hold M5: try again", x, 105);
    canvas.drawString("Hold top: Wi-Fi", x, 119);
  } else {
    canvas.drawString("Hold M5: try again", x, 119);
  }
}

// Shortens `text` with "..." until it fits `maxW` in the current font.
static String fitText(String text, int maxW)
{
  if (canvas.textWidth(text) <= maxW) return text;
  while (text.length() && canvas.textWidth(text + "...") > maxW) text.remove(text.length() - 1);
  return text + "...";
}

static void drawSignal(int x, int bottom, int rssi, uint16_t on, uint16_t off)
{
  int bars = rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
  for (int b = 0; b < 3; b++) {
    int h = 3 + b * 3;
    canvas.fillRect(x + b * 4, bottom - h, 3, h, b < bars ? on : off);
  }
}

static void drawLock(int x, int y, uint16_t color)
{
  canvas.fillArc(x + 4, y + 4, 2, 3, 180, 360, color);  // shackle
  canvas.fillSmoothRoundRect(x, y + 4, 9, 7, 1, color);  // body
}

static void drawWifiMenu()
{
  canvas.setTextDatum(top_left);
  canvas.setFont(FONT_SUBTITLE);
  canvas.setTextColor(TEXT);
  canvas.drawString("Wi-Fi", 8, BAR_H + 3);

  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(MUTED);
  canvas.setTextDatum(top_right);
  if (scanning) {
    float a = fmodf(seconds() * 360.0f, 360.0f);
    int cx = W - 14, cy = BAR_H + 10;
    canvas.fillArc(cx, cy, 4, 6, a, min(a + 120, 360.0f), ACCENT);
    if (a + 120 > 360) canvas.fillArc(cx, cy, 4, 6, 0, a + 120 - 360, ACCENT);
    canvas.drawString("Scanning", W - 26, BAR_H + 5);
  } else {
    canvas.drawString(String(scanList.size()) + " nearby", W - 8, BAR_H + 5);
  }

  const int top = BAR_H + 21, rowH = 20, rows = 4;
  const int n = scanList.size();
  if (n == 0) {
    canvas.setTextDatum(middle_center);
    canvas.drawString(scanning ? "Looking for networks..." : "No networks found", W / 2, top + rowH * 2);
  }
  if (wifiSel < wifiTop) wifiTop = wifiSel;
  if (wifiSel >= wifiTop + rows) wifiTop = wifiSel - rows + 1;

  String current = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  for (int r = 0; r < rows && wifiTop + r < n; r++) {
    int i = wifiTop + r, y = top + r * rowH;
    const ScanEntry &e = scanList[i];
    bool sel = i == wifiSel;
    bool on = e.ssid == current;
    bool usable = on || e.open || e.access != ACCESS_NONE;
    if (sel) {
      canvas.fillSmoothRoundRect(4, y, W - 14, rowH - 2, 6, SURFACE);
      canvas.fillSmoothRoundRect(4, y + 5, 3, rowH - 12, 1, ACCENT);
    }
    drawSignal(13, y + 14, e.rssi, usable ? TEXT : MUTED, sel ? BG : SURFACE);

    String tag = on ? "on" : e.access == ACCESS_SAVED ? "saved" : e.open ? "open" : "";
    int tagW = tag.length() ? canvas.textWidth(tag) + 10 : 18;
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(usable ? TEXT : MUTED);
    canvas.drawString(fitText(toAscii(e.ssid.c_str()), W - 30 - tagW - 16), 30, y + rowH / 2 - 1);

    canvas.setTextDatum(middle_right);
    if (tag.length()) {
      canvas.setTextColor(on ? ACCENT : MUTED);
      canvas.drawString(tag, W - 16, y + rowH / 2 - 1);
      if (on) canvas.fillSmoothCircle(W - 20 - canvas.textWidth(tag), y + rowH / 2 - 1, 2, ACCENT);
    } else {
      drawLock(W - 26, y + 3, MUTED);
    }
  }

  if (n > rows) {
    int trackH = rows * rowH - 2;
    int thumbH = max(8, trackH * rows / n);
    int thumbY = top + (trackH - thumbH) * wifiTop / (n - rows);
    canvas.fillRect(W - 4, top, 2, trackH, SURFACE);
    canvas.fillRect(W - 4, thumbY, 2, thumbH, ACCENT);
  }

  canvas.setTextDatum(bottom_center);
  canvas.setTextColor(MUTED);
  bool firstHint = ((millis() / 2500) % 2) == 0;
  canvas.drawString(firstHint ? "Top / bottom: move" : "Hold M5: join   Click M5: home", W / 2, H - 1);
}

static void drawInfo()
{
  // Time and date
  struct tm t;
  bool synced = getLocalTime(&t, 0) && t.tm_year >= 125;
  char clock[8] = "--:--", date[32] = "Time not synced yet";
  if (synced) {
    strftime(clock, sizeof(clock), "%H:%M", &t);
    strftime(date, sizeof(date), "%A, %e %B", &t);
  }
  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::FreeSansBold24pt7b);
  canvas.setTextColor(TEXT);
  canvas.drawString(clock, 10, 8);
  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(MUTED);
  String d = date;
  d.trim();  // %e pads single-digit days with a space
  d.replace("  ", " ");
  canvas.drawString(d, 12, 52);

  // Battery
  if (batteryPct < 0 || millis() - lastBatteryRead > 5000) {
    batteryPct = M5.Power.getBatteryLevel();
    lastBatteryRead = millis();
  }
  int bx = 170, by = 14;
  canvas.drawRoundRect(bx, by, 44, 20, 4, MUTED);
  canvas.fillRect(bx + 44, by + 6, 3, 8, MUTED);
  canvas.fillSmoothRoundRect(bx + 3, by + 3, max(2, constrain(batteryPct, 0, 100) * 38 / 100), 14, 2,
                             batteryPct <= 20 ? DANGER : ACCENT);
  canvas.setTextDatum(top_center);
  canvas.setFont(FONT_SUBTITLE);
  canvas.setTextColor(TEXT);
  canvas.drawString(String(batteryPct) + "%", bx + 22, by + 26);
  canvas.setFont(FONT_SMALL);
  canvas.setTextColor(MUTED);
  canvas.drawString(String(M5.Power.getBatteryVoltage() / 1000.0f, 2) + " V", bx + 22, by + 44);

  // Wi-Fi
  canvas.drawFastHLine(10, 74, W - 20, SURFACE);
  canvas.setTextDatum(top_left);
  if (WiFi.status() == WL_CONNECTED) {
    drawSignal(12, 94, WiFi.RSSI(), TEXT, SURFACE);
    canvas.setFont(FONT_BODY);
    canvas.setTextColor(TEXT);
    canvas.drawString(fitText(toAscii(WiFi.SSID().c_str()), W - 40), 30, 80);
    canvas.setFont(FONT_SMALL);
    canvas.setTextColor(MUTED);
    canvas.drawString(WiFi.localIP().toString() + "   " + String(WiFi.RSSI()) + " dBm", 30, 100);
  } else {
    drawSignal(12, 94, -100, SURFACE, SURFACE);
    canvas.setFont(FONT_BODY);
    canvas.setTextColor(MUTED);
    canvas.drawString("Wi-Fi not connected", 30, 80);
  }

  // Bottom bar: time left on this screen, or power-off progress while the
  // bottom button is still held.
  if (!M5.BtnPWR.isPressed()) {  // while held, the hold timer owns the bottom edge
    int barY = H - 6, barW = W - 20;
    float left = infoUntil > millis() ? (infoUntil - millis()) / (float)INFO_MS : 0;
    canvas.fillSmoothRoundRect(10, barY, barW, 4, 2, SURFACE);
    canvas.fillSmoothRoundRect(10, barY, max(4, (int)(barW * left)), 4, 2, ACCENT);
  }
}

static void drawBye()
{
  canvas.setFont(FONT_TITLE);
  canvas.setTextColor(TEXT);
  canvas.setTextDatum(middle_center);
  canvas.drawString("Bye", W / 2, H / 2);
}

// While a side button is held, a bar along its edge of the screen counts down
// to what the long press will do, so nobody powers off by accident.
static void drawHoldBar(bool atTop, float progress, uint16_t color, const String &label)
{
  int barY = atTop ? 0 : H - 3;
  canvas.fillRect(0, barY, W, 3, SURFACE);
  canvas.fillRect(0, barY, (int)(W * constrain(progress, 0.0f, 1.0f)), 3, color);
  canvas.setFont(FONT_SMALL);
  int w = canvas.textWidth(label) + 16;
  int y = atTop ? 6 : H - 24;
  canvas.fillSmoothRoundRect((W - w) / 2, y, w, 17, 8, BG);
  canvas.drawRoundRect((W - w) / 2, y, w, 17, 8, color);
  canvas.setTextColor(color);
  canvas.setTextDatum(middle_center);
  canvas.drawString(label, W / 2, y + 8);
}

static String secondsLeft(uint32_t ms)
{
  return String(ms / 1000.0f, 1) + "s";
}

static void drawHoldTimers()
{
  if (swallowPress) return;  // that press only woke the screen
  const uint32_t showAfter = 150;  // don't flash for an ordinary click

  auto &top = M5.BtnB;
  uint32_t topHeld = top.isPressed() ? top.getUpdateMsec() - top.lastChange() : 0;
  bool topHolds = screen == Screen::Home || screen == Screen::Answer || screen == Screen::Error ||
                  screen == Screen::Connecting || screen == Screen::Wifi;
  if (topHolds && topHeld > showAfter && topHeld < TOP_HOLD_MS + 600) {
    const char *what = screen == Screen::Wifi ? "Back" : "Wi-Fi";
    String label = topHeld < TOP_HOLD_MS ? String(what) + " in " + secondsLeft(TOP_HOLD_MS - topHeld) : String(what);
    drawHoldBar(true, topHeld / (float)TOP_HOLD_MS, ACCENT, label);
  }

  auto &bottom = M5.BtnPWR;
  uint32_t botHeld = bottom.isPressed() ? bottom.getUpdateMsec() - bottom.lastChange() : 0;
  if (botHeld > showAfter) {
    bool infoHolds = screen == Screen::Home || screen == Screen::Answer || screen == Screen::Error ||
                     screen == Screen::Connecting || screen == Screen::Info;
    if (botHeld < INFO_HOLD_MS) {
      if (infoHolds) drawHoldBar(false, botHeld / (float)INFO_HOLD_MS, ACCENT, "Info in " + secondsLeft(INFO_HOLD_MS - botHeld));
    } else {
      float p = (botHeld - INFO_HOLD_MS) / (float)(POWER_OFF_MS - INFO_HOLD_MS);
      drawHoldBar(false, p, DANGER, "Power off in " + secondsLeft(POWER_OFF_MS - min(botHeld, POWER_OFF_MS)));
    }
  }
}

static void render()
{
  canvas.fillSprite(BG);
  if (screen != Screen::Bye && screen != Screen::Info) drawStatusBar();
  // An M5 press records from the start, but until it has lasted long enough
  // to be a question, keep showing the screen it began on: it may be a click.
  Screen shown = screen == Screen::Listening && !sampleListening && recSamples < MIN_SAMPLES ? screenBeforeListening : screen;
  switch (shown) {
    case Screen::Connecting: drawConnecting(); break;
    case Screen::Home: drawHome(); break;
    case Screen::Listening: drawListening(); break;
    case Screen::Working: drawWorking(); break;
    case Screen::Answer: drawAnswer(); break;
    case Screen::Error: drawError(); break;
    case Screen::Wifi: drawWifiMenu(); break;
    case Screen::Info: drawInfo(); break;
    case Screen::Bye: drawBye(); break;
  }
  drawToast();
  if (screen != Screen::Bye) drawHoldTimers();
  canvas.pushSprite(0, 0);
}

static void showError(const String &title, const String &text, bool wifi = false)
{
  errorTitle = title;
  errorText = text;
  errorIsWifi = wifi;
  setScreen(Screen::Error);
}

// ---- Wi-Fi and time ----

static WiFiMulti wifiMulti;
static int wifiNetworkCount = 0;

// What secrets.h knows about `ssid`; `pass` gets its password ("" if open).
static Access savedAccess(const String &ssid, const char **pass)
{
  for (auto &n : WIFI_NETWORKS) {
    if (ssid != n.ssid) continue;
    if (n.pass == WIFI_OPEN) { *pass = ""; return ACCESS_OPEN; }
    if (n.pass[0]) { *pass = n.pass; return ACCESS_SAVED; }
  }
  return ACCESS_NONE;
}

static void setupWifi()
{
  for (auto &n : WIFI_NETWORKS) {
    if (n.pass == WIFI_OPEN) wifiMulti.addAP(n.ssid, nullptr);
    else if (n.pass[0]) wifiMulti.addAP(n.ssid, n.pass);
    else continue;  // no password filled in yet
    wifiNetworkCount++;
  }
  WiFi.mode(WIFI_STA);
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
}

// Starts a background connection attempt; the Connecting screen watches it.
static void startWifi()
{
  connectingLabel = "Looking for Wi-Fi";
  joinSsid = "";
  connectResult.store(0);
  job.store(JOB_CONNECT);
  xTaskNotifyGive(netTask);
  setScreen(Screen::Connecting);
}

// ---- Called from the network task ----

static void waitForScan()
{
  uint32_t t0 = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t0 < 6000) delay(50);
}

// Joins one specific network. `pass` may be empty for an open network.
static bool joinNetwork(const String &ssid, const char *pass, uint32_t timeoutMs)
{
  waitForScan();
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid.c_str(), pass && pass[0] ? pass : nullptr);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs && !cancelConnect.load()) {
    wl_status_t st = WiFi.status();
    if (millis() - t0 > 2000 && (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED)) break;
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Tries the network picked last time in the Wi-Fi menu, then lets WiFiMulti
// scan and join the strongest listed network in range.
static bool connectWifi(uint32_t timeoutMs)
{
  String preferred = prefs.getString("ssid", "");
  const char *pass;
  if (preferred.length() && savedAccess(preferred, &pass) != ACCESS_NONE && joinNetwork(preferred, pass, 8000))
    return true;
  if (!wifiNetworkCount) return false;
  waitForScan();
  uint32_t t0 = millis();
  while (wifiMulti.run(8000) != WL_CONNECTED) {
    if (millis() - t0 > timeoutMs || cancelConnect.load()) return false;
    delay(250);
  }
  return true;
}

static String localNow()
{
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 125) return "";
  char buf[40];
  strftime(buf, sizeof(buf), "%A %Y-%m-%d %H:%M %Z", &t);
  return buf;
}

// ---- Audio ----

static float chunkLevel(const int16_t *p, size_t n)
{
  double sum = 0;
  for (size_t i = 0; i < n; i++) sum += (double)p[i] * p[i];
  float rms = sqrtf(sum / n);
  return min(1.0f, sqrtf(rms / 2500.0f));
}

// Scales the recording up so the loudest sample sits near 70% of full range.
// Returns false if it's essentially silence.
static bool normalize(size_t n)
{
  int peak = 0;
  for (size_t i = 0; i < n; i++) peak = max(peak, abs((int)pcm[i]));
  if (peak < 400) return false;
  float gain = min(23000.0f / peak, 8.0f);
  if (gain > 1.05f)
    for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)(pcm[i] * gain);
  return true;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static void writeWavHeader(uint8_t *h, size_t samples)
{
  uint32_t dataLen = samples * 2;
  memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + dataLen);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1);  // PCM
  put16(h + 22, 1);  // mono
  put32(h + 24, SAMPLE_RATE);
  put32(h + 28, SAMPLE_RATE * 2);
  put16(h + 32, 2);
  put16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  put32(h + 40, dataLen);
}

// ---- Speech-to-text (Whisper on Groq) ----

static bool transcribe(size_t samples, String &text, String &err)
{
  String pre;
  pre += "--"; pre += BOUNDARY; pre += "\r\n";
  pre += "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-large-v3-turbo\r\n";
  pre += "--"; pre += BOUNDARY; pre += "\r\n";
  pre += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";
  pre += "--"; pre += BOUNDARY; pre += "\r\n";
  pre += "Content-Disposition: form-data; name=\"file\"; filename=\"question.wav\"\r\n";
  pre += "Content-Type: audio/wav\r\n\r\n";
  String post = String("\r\n--") + BOUNDARY + "--\r\n";

  uint8_t *wav = uploadBuf + PRE_ROOM;
  writeWavHeader(wav, samples);
  uint8_t *body = wav - pre.length();
  memcpy(body, pre.c_str(), pre.length());
  uint8_t *end = wav + WAV_HEADER + samples * 2;
  memcpy(end, post.c_str(), post.length());
  size_t bodyLen = (end + post.length()) - body;

  WiFiClientSecure tls;
  tls.setCACert(CA_PEM);
  HTTPClient http;
  http.setTimeout(30000);
  if (!http.begin(tls, "https://api.groq.com/openai/v1/audio/transcriptions")) {
    err = "Couldn't reach Groq.";
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);
  int code = http.POST(body, bodyLen);
  String resp = code > 0 ? http.getString() : "";
  http.end();

  JsonDocument doc;
  deserializeJson(doc, resp);
  if (code != 200) {
    err = "Speech-to-text failed (" + String(code) + "). " + (const char *)(doc["error"]["message"] | "");
    return false;
  }
  text = (const char *)(doc["text"] | "");
  text.trim();
  return true;
}

// ---- Web search (Tavily) ----

// Searches the web for `question` and returns compact results as plain text
// for the model to read: Tavily's short answer, then a snippet per page.
// Returns the HTTP status (or a negative HTTPClient error).
static int tavilySearch(const String &question, String &results, String &err)
{
  JsonDocument req;
  req["query"] = question;
  req["search_depth"] = "basic";  // 1 credit; the free plan has 1,000 a month
  req["max_results"] = 5;
  req["include_answer"] = "basic";
  String body;
  serializeJson(req, body);

  WiFiClientSecure tls;
  tls.setCACert(CA_PEM);
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(20000);
  if (!http.begin(tls, "https://api.tavily.com/search")) {
    err = "Couldn't reach Tavily.";
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + TAVILY_API_KEY);
  int code = http.POST(body);

  JsonDocument filter;
  filter["answer"] = true;
  filter["results"][0]["title"] = true;
  filter["results"][0]["content"] = true;
  filter["detail"] = true;  // Tavily's error message
  JsonDocument doc;
  if (code > 0) deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (code != 200) {
    JsonVariant detail = doc["detail"];
    String msg = detail.is<const char *>() ? detail.as<String>() : (const char *)(detail["error"] | "");
    err = "Tavily error (" + String(code) + "). " + msg;
    return code;
  }

  results = "";
  const char *quick = doc["answer"] | "";
  if (*quick) results += String("Search engine summary: ") + quick + "\n\n";
  int i = 1;
  for (JsonObject r : doc["results"].as<JsonArray>()) {
    String item = "[" + String(i++) + "] " + (const char *)(r["title"] | "") + "\n" + (const char *)(r["content"] | "") + "\n\n";
    if (results.length() + item.length() > 6000) break;  // keep the prompt small for Groq's free limits
    results += item;
  }
  return code;
}

// ---- Answer (GPT-OSS on Groq) ----

static const char *GROQ_MODEL = "openai/gpt-oss-120b";

static const char *SYSTEM_PROMPT =
  "You are Jarvis, a pocket search assistant whose answers appear on a tiny 240x135 pixel "
  "screen. The question was spoken aloud and transcribed by speech-to-text, so it may contain "
  "transcription errors; interpret it sensibly. When web search results come with the "
  "question, they are newer than your own knowledge: if they disagree with what you remember, "
  "trust the results. Reply with the answer only: one to three short sentences, under 60 "
  "words. Plain ASCII text with no markdown, tables, bullet points, citation markers, URLs or "
  "emoji, and no preamble. Write units and symbols as words (say 'degrees', not the symbol).";

// One chat completion. `results` is the web search text, or empty to answer
// from the model's own knowledge. Returns the HTTP status (or a negative
// HTTPClient error).
static int groqChat(const String &question, const String &results, String &answer, String &err)
{
  JsonDocument req;
  req["model"] = GROQ_MODEL;
  req["max_completion_tokens"] = 8192;
  req["reasoning_effort"] = "high";
  String system = SYSTEM_PROMPT;
  String now = localNow();
  if (!now.isEmpty()) system += " The user's local time is " + now + ".";
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = system;
  JsonObject user = msgs.add<JsonObject>();
  user["role"] = "user";
  user["content"] = results.isEmpty() ? question
                                      : "Question: " + question + "\n\nWeb search results:\n\n" + results;
  String body;
  serializeJson(req, body);

  WiFiClientSecure tls;
  tls.setCACert(CA_PEM);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the body parses straight off the socket
  http.setTimeout(60000);
  if (!http.begin(tls, "https://api.groq.com/openai/v1/chat/completions")) {
    err = "Couldn't reach Groq.";
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);
  int code = http.POST(body);

  // The response also carries the model's reasoning; keep only the answer.
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["error"]["message"] = true;
  JsonDocument doc;
  if (code > 0) deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (code != 200) {
    err = "Groq error (" + String(code) + "). " + (const char *)(doc["error"]["message"] | "");
    return code;
  }
  answer = (const char *)(doc["choices"][0]["message"]["content"] | "");
  answer.trim();
  if (answer.isEmpty()) err = "No answer came back.";
  return code;
}

// ---- Network task ----

static bool cancelled() { return jobSeq.load() != runningJob; }

// Publishes progress for the current question, unless it was cancelled.
static void publish(Stage st)
{
  if (!cancelled()) stage.store(st, std::memory_order_release);
}

static void fail(const String &title, const String &text)
{
  if (cancelled()) return;
  jobErrorTitle = title;
  jobError = text;
  stage.store(ST_ERROR, std::memory_order_release);
}

static void runJob()
{
  runningJob = jobSeq.load();
  if (WiFi.status() != WL_CONNECTED && !connectWifi(15000)) {
    fail("No Wi-Fi", "None of your saved networks are in range.");
    return;
  }

  if (!strcmp(GROQ_API_KEY, "gsk_...")) {
    fail("No API key", "Paste your free key from console.groq.com/keys into secrets.h, then flash again.");
    return;
  }

  String question, err;
  if (!transcribe(jobSamples, question, err)) {
    if (err.indexOf("(401)") >= 0) fail("Bad API key", "Groq didn't accept the key in secrets.h. Check it was copied in full.");
    else fail("Couldn't transcribe", err);
    return;
  }
  if (question.isEmpty()) {
    fail("No words heard", "Speak a little closer to the stick and try again.");
    return;
  }
  if (cancelled()) return;
  Serial.printf("Q: %s\n", question.c_str());
  jobQuestion = question;
  publish(ST_SEARCH);

  // Search first; if that fails, answer from the model's own knowledge and
  // tag the answer "no web".
  String results, answer;
  if (!strcmp(TAVILY_API_KEY, "tvly-...")) {
    Serial.println("No Tavily key: answering without web search");
  } else {
    int code = tavilySearch(question, results, err);
    if (code != 200) Serial.printf("Search failed: %s\n", err.c_str());
  }
  if (cancelled()) return;
  publish(ST_THINK);

  int code = groqChat(question, results, answer, err);
  if (code == 429) {
    // Groq says how long to wait, e.g. "Please try again in 1m12.5s."
    int at = err.indexOf("try again in ");
    String wait = at >= 0 ? err.substring(at + 13, err.indexOf('.', at + 13)) : "a minute";
    fail("Slow down", "Groq's free limit is used up for now. Wait " + wait + " and ask again.");
    return;
  }
  if (code != 200 || answer.isEmpty()) {
    fail(code == 401 ? "Bad API key" : "No answer", err);
    return;
  }
  if (cancelled()) return;
  Serial.printf("A (%s): %s\n", results.isEmpty() ? "no web" : "web", answer.c_str());
  jobAnswer = answer;
  jobSearched = !results.isEmpty();
  publish(ST_DONE);
}

static void netTaskMain(void *)
{
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    netBusy.store(true);
    cancelConnect.store(false);
    switch (job.load()) {
      case JOB_CONNECT:
        connectResult.store(connectWifi(20000) ? 1 : 2);
        break;
      case JOB_JOIN: {
        bool ok = joinNetwork(joinSsid, joinPass.c_str(), 15000);
        if (ok) prefs.putString("ssid", joinSsid);  // try it first next time
        connectResult.store(ok ? 1 : 2);
        break;
      }
      default:
        runJob();
    }
    netBusy.store(false);
  }
}

// ---- Input ----

// Returns true if the screen was dim or off.
static bool wake()
{
  lastActivity = millis();
  if (backlight == Backlight::On) return false;
  if (backlight == Backlight::Off) M5.Display.wakeup();
  M5.Display.setBrightness(BRIGHT);
  backlight = Backlight::On;
  return true;
}

static void updateBacklight(bool busy)
{
  if (busy) lastActivity = millis();
  uint32_t idle = millis() - lastActivity;
  if (backlight == Backlight::On && idle > 30000) {
    M5.Display.setBrightness(DIM);
    backlight = Backlight::Dim;
  } else if (backlight == Backlight::Dim && idle > 120000) {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    backlight = Backlight::Off;
  }
}

static void startListening()
{
  M5.Mic.begin();
  sampleListening = false;
  recSamples = 0;
  level = 0;
  for (auto &l : levels) l = 0;
  setScreen(Screen::Listening);
}

static void finishListening()
{
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();

  if (recSamples < MIN_SAMPLES) {  // a click, not a question
    // On the home screen a tap opens the last answer; everywhere else it goes home.
    setScreen(screenBeforeListening == Screen::Home && haveAnswer ? Screen::Answer : Screen::Home);
    return;
  }
  if (!normalize(recSamples)) {
    setScreen(screenBeforeListening);
    showToast("I didn't hear anything");
    return;
  }
  jobSamples = recSamples;
  stage.store(ST_TRANSCRIBE, std::memory_order_release);
  setScreen(Screen::Working);
  job.store(JOB_ASK);
  xTaskNotifyGive(netTask);
}

static void updateListening()
{
  if (M5.BtnA.isPressed() && recSamples + CHUNK <= MAX_SAMPLES) {
    // Keep two requests queued so the capture is continuous.
    while (M5.Mic.isRecording() < 2 && recSamples + CHUNK <= MAX_SAMPLES) {
      if (!M5.Mic.record(pcm + recSamples, CHUNK, SAMPLE_RATE)) break;
      recSamples += CHUNK;
    }
    // The newest finished chunk sits behind the ones still queued.
    size_t done = recSamples - M5.Mic.isRecording() * CHUNK;
    if (done >= CHUNK && millis() - lastLevelPush > 60) {
      lastLevelPush = millis();
      float l = chunkLevel(pcm + done - CHUNK, CHUNK);
      level = level * 0.4f + l * 0.6f;
      const int n = sizeof(levels) / sizeof(levels[0]);
      memmove(levels, levels + 1, (n - 1) * sizeof(float));
      levels[n - 1] = level;
    }
    return;
  }
  finishListening();
}

// ---- Wi-Fi menu ----

static void startScan()
{
  if (netBusy.load()) {  // the network task is mid-connect; scan when it's done
    scanPending = true;
    return;
  }
  scanPending = false;
  lastScan = millis();
  scanning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
  if (!scanning) lastScan = millis() - 13000;  // retry in 2 s
}

static void pollScan()
{
  if (!scanning) {
    if ((scanPending || millis() - lastScan > 20000) && !netBusy.load()) startScan();
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  // As a scan finishes, the library briefly reports "failed" before the
  // results land, so only give up on a scan that's been going too long.
  if (n == WIFI_SCAN_FAILED && millis() - lastScan < 15000) return;
  scanning = false;
  if (n < 0) {
    lastScan = millis() - 13000;  // retry in 2 s
    return;
  }

  String selected = wifiSel < (int)scanList.size() ? scanList[wifiSel].ssid : "";
  scanList.clear();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // hidden network
    int rssi = WiFi.RSSI(i);
    auto same = std::find_if(scanList.begin(), scanList.end(), [&](const ScanEntry &e) { return e.ssid == ssid; });
    if (same != scanList.end()) {  // several access points, one name: keep the strongest
      same->rssi = max(same->rssi, rssi);
      continue;
    }
    const char *pass;
    scanList.push_back({ssid, rssi, WiFi.encryptionType(i) == WIFI_AUTH_OPEN, savedAccess(ssid, &pass)});
  }
  WiFi.scanDelete();

  // Connected first, then networks Jarvis can join, each by signal strength.
  String current = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  auto rank = [&](const ScanEntry &e) { return e.ssid == current ? 0 : (e.open || e.access != ACCESS_NONE) ? 1 : 2; };
  std::sort(scanList.begin(), scanList.end(), [&](const ScanEntry &a, const ScanEntry &b) {
    return rank(a) != rank(b) ? rank(a) < rank(b) : a.rssi > b.rssi;
  });

  wifiSel = 0;
  for (size_t i = 0; i < scanList.size(); i++)
    if (scanList[i].ssid == selected) wifiSel = i;
}

static void openWifiMenu()
{
  if (screen != Screen::Wifi) screenBeforeWifi = screen == Screen::Answer ? Screen::Answer : Screen::Home;
  if (netBusy.load() && job.load() == JOB_CONNECT) cancelConnect.store(true);  // stop hunting so the scan can run
  setScreen(Screen::Wifi);
  startScan();
}

static void joinSelected()
{
  if (scanList.empty()) return;
  const ScanEntry &e = scanList[wifiSel];
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == e.ssid) {
    showToast("Already on this one");
    return;
  }
  const char *pass = "";
  if (savedAccess(e.ssid, &pass) == ACCESS_NONE && !e.open) {
    showToast("No password in secrets.h");
    return;
  }
  if (netBusy.load()) {
    showToast("Busy, try again");
    return;
  }
  joinSsid = e.ssid;
  joinPass = pass;
  connectingLabel = "Joining " + toAscii(e.ssid.c_str());
  connectResult.store(0);
  job.store(JOB_JOIN);
  xTaskNotifyGive(netTask);
  setScreen(Screen::Connecting);
}

static void scrollBy(int dy)
{
  scrollTarget = constrain(scrollTarget + dy, 0, maxScroll());
}

static void openInfo()
{
  if (screen != Screen::Info) infoReturn = screen;
  infoUntil = millis() + INFO_MS;
  setScreen(Screen::Info);
}

static void handleButtons()
{
  // Held with the M5 button on the right, the side button is on top and the
  // power button at the bottom.
  auto &top = M5.BtnB;
  auto &bottom = M5.BtnPWR;

  bool any = M5.BtnA.wasPressed() || top.wasPressed() || bottom.wasPressed();
  if (any && wake()) swallowPress = true;
  if (swallowPress) {  // ignore the waking press until every button is up
    if (!M5.BtnA.isPressed() && !top.isPressed() && !bottom.isPressed()) swallowPress = false;
    return;
  }

  if (top.wasClicked()) Serial.println("btn: top click");
  if (top.wasHold()) Serial.println("btn: top hold");
  if (bottom.wasClicked()) Serial.println("btn: bottom click");
  if (bottom.wasHold()) Serial.println("btn: bottom hold");

  if (bottom.pressedFor(POWER_OFF_MS)) {
    setScreen(Screen::Bye);
    render();
    delay(700);
    M5.Power.powerOff();
  }

  if (screen == Screen::Info) {
    if (M5.BtnA.wasPressed()) {  // records from the press; a click ends on the home screen
      screenBeforeListening = Screen::Info;
      startListening();
    } else if (top.wasClicked() || bottom.wasClicked()) {
      setScreen(infoReturn);
    }
    return;
  }

  if (screen == Screen::Wifi) {
    int n = scanList.size();
    if (top.wasHold()) {
      setScreen(WiFi.status() == WL_CONNECTED ? screenBeforeWifi : Screen::Home);
    } else if (n && top.wasClicked()) {
      wifiSel = (wifiSel + n - 1) % n;
    } else if (n && bottom.wasClicked()) {
      wifiSel = (wifiSel + 1) % n;
    } else if (M5.BtnA.wasHold()) {
      joinSelected();
    } else if (M5.BtnA.wasClicked()) {
      setScreen(Screen::Home);
    }
    return;
  }

  if (screen == Screen::Working) {  // three quick M5 taps cancel the question
    static int taps = 0;
    static uint32_t lastTap = 0;
    if (M5.BtnA.wasClicked()) {
      if (millis() - lastTap > 800) taps = 0;
      lastTap = millis();
      if (++taps >= 3) {
        taps = 0;
        jobSeq++;
        stage.store(ST_IDLE);
        setScreen(Screen::Home);
        showToast("Cancelled");
      } else {
        showToast(taps == 1 ? "Tap M5 2 more times to cancel" : "Once more to cancel");
      }
    }
    return;
  }

  bool idle = screen == Screen::Home || screen == Screen::Answer || screen == Screen::Error ||
              screen == Screen::Connecting;
  if (idle && top.wasHold()) {
    openWifiMenu();
    return;
  }
  if (idle && bottom.wasHold()) {
    openInfo();
    return;
  }

  switch (screen) {
    case Screen::Connecting:
    case Screen::Listening:
    case Screen::Working:
    case Screen::Bye:
      return;
    case Screen::Error:
      if (WiFi.status() != WL_CONNECTED) {  // no point recording: hold retries Wi-Fi
        if (M5.BtnA.wasHold()) startWifi();
        else if (M5.BtnA.wasClicked()) setScreen(Screen::Home);
        return;
      }
      break;
    default:
      break;
  }

  if (M5.BtnA.wasPressed()) {  // records from the press; a click ends on the home screen
    screenBeforeListening = screen;
    startListening();
    return;
  }

  bool click = top.wasClicked() || bottom.wasClicked();
  if (screen == Screen::Home && haveAnswer && click) {
    setScreen(Screen::Answer);
    return;
  }
  if (screen == Screen::Error && click) {
    setScreen(haveAnswer ? Screen::Answer : Screen::Home);
    return;
  }
  if (screen == Screen::Answer) {
    int step = VIEW_H * 3 / 4;
    if (top.wasClicked()) {
      if (scrollTarget == 0) showToast("That's the top");
      scrollBy(-step);
    }
    if (bottom.wasClicked()) {
      if (scrollTarget >= maxScroll()) showToast(maxScroll() ? "That's the end" : "That's all of it");
      scrollBy(step);
    }
  }
}

// ---- Serial debug commands ----
// "shot" dumps the screen as hex RGB565; "show <screen>" jumps to a screen
// with sample content (home, listen, work, answer, error), for checking the UI.

static void dumpScreen()
{
  const uint8_t *px = (const uint8_t *)canvas.getBuffer();
  Serial.printf("SHOT %d %d\n", W, H);
  const size_t total = (size_t)W * H * 2;
  char hex[2 * 64 + 1];
  for (size_t i = 0; i < total; i += 64) {
    size_t n = min((size_t)64, total - i);
    for (size_t j = 0; j < n; j++) sprintf(hex + 2 * j, "%02x", px[i + j]);
    hex[2 * n] = 0;
    Serial.println(hex);
  }
  Serial.println("END");
}

static void showSample(const String &name)
{
  if (name == "home") {
    setScreen(Screen::Home);
  } else if (name == "listen") {
    setScreen(Screen::Listening);
    sampleListening = true;
    recSamples = SAMPLE_RATE * 4;
    for (int i = 0; i < 22; i++) levels[i] = 0.25f + 0.6f * fabsf(sinf(i * 0.7f)) * (i % 3 ? 1 : 0.5f);
    level = levels[21];
  } else if (name == "work") {
    jobQuestion = "Who won the Formula 1 race last weekend and by how much?";
    stage.store(ST_SEARCH);
    setScreen(Screen::Working);
  } else if (name == "answer") {
    layoutAnswer("How tall is the Eiffel Tower?",
                 "The Eiffel Tower is 330 metres (1,083 feet) tall including its antennas. "
                 "It was the tallest man-made structure in the world until the Chrysler "
                 "Building in New York was finished in 1930.");
    haveAnswer = true;
    setScreen(Screen::Answer);
  } else if (name == "info") {
    openInfo();
    infoUntil = millis() + 60000;  // hold it for the screenshot
  } else if (name == "wifi") {
    openWifiMenu();
  } else if (name == "error") {
    showError("Slow down", "Groq's free limit was hit. Wait a minute and ask again.");
  }
}

static void handleSerial()
{
  static String cmd;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      cmd += c;
      continue;
    }
    cmd.trim();
    if (cmd == "shot") {
      render();
      dumpScreen();
    } else if (cmd == "wifi") {
      Serial.printf("scan: scanning=%d complete=%d listed=%u netBusy=%d sinceScan=%lu status=%d\n",
                    scanning, WiFi.scanComplete(), (unsigned)scanList.size(), netBusy.load(),
                    (unsigned long)(millis() - lastScan), WiFi.status());
    } else if (cmd.startsWith("show ")) {
      showSample(cmd.substring(5));
    }
    cmd = "";
  }
}

// ---- Arduino entry points ----

void setup()
{
  auto cfg = M5.config();
  cfg.internal_spk = false;  // the buzzer isn't used
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.printf("boot: reset reason %d\n", (int)esp_reset_reason());

  M5.Display.setRotation(1);
  M5.Display.setBrightness(BRIGHT);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(W, H);

  uploadBuf = (uint8_t *)ps_malloc(PRE_ROOM + WAV_HEADER + MAX_SAMPLES * 2 + POST_ROOM);
  if (!uploadBuf) {
    showError("No PSRAM", "Couldn't allocate the audio buffer.");
    render();
    while (true) delay(1000);
  }
  pcm = (int16_t *)(uploadBuf + PRE_ROOM + WAV_HEADER);

  auto mic = M5.Mic.config();
  mic.sample_rate = SAMPLE_RATE;
  M5.Mic.config(mic);

  prefs.begin("jarvis");
  M5.BtnPWR.setHoldThresh(INFO_HOLD_MS);
  M5.BtnB.setHoldThresh(TOP_HOLD_MS);

  // TLS handshakes need a deep stack.
  xTaskCreatePinnedToCore(netTaskMain, "net", 16384, nullptr, 1, &netTask, 0);

  setupWifi();
  startWifi();
  lastActivity = millis();
}

void loop()
{
  M5.update();
  handleSerial();
  handleButtons();

  if (screen == Screen::Listening && !sampleListening) updateListening();
  if (screen == Screen::Wifi) pollScan();
  if (screen == Screen::Info && millis() > infoUntil && !M5.BtnPWR.isPressed()) setScreen(infoReturn);

  if (screen == Screen::Connecting) {
    int r = connectResult.load();
    if (r == 1) {
      setScreen(Screen::Home);
      showToast("On " + toAscii(WiFi.SSID().c_str()));
    } else if (r == 2 && joinSsid.length()) {
      showError("Couldn't join", toAscii(joinSsid.c_str()) + ": wrong password or weak signal.", true);
    } else if (r == 2 && !wifiNetworkCount) {
      showError("No Wi-Fi set up", "Add passwords to secrets.h, or pick an open network.", true);
    } else if (r == 2) {
      showError("No Wi-Fi", "None of your saved networks are in range.", true);
    }
  }

  if (screen == Screen::Working) {
    int st = stage.load(std::memory_order_acquire);
    if (st == ST_DONE) {
      layoutAnswer(jobQuestion, jobAnswer);
      answerSearched = jobSearched;
      haveAnswer = true;
      stage.store(ST_IDLE);
      setScreen(Screen::Answer);
      wake();
    } else if (st == ST_ERROR) {
      stage.store(ST_IDLE);
      showError(jobErrorTitle, toAscii(jobError.c_str()));
      wake();
    }
  }

  updateBacklight(screen == Screen::Listening || screen == Screen::Working || screen == Screen::Connecting);

  static uint32_t lastFrame = 0;
  uint32_t frameMs = backlight == Backlight::On ? 33 : 500;
  if (backlight != Backlight::Off && millis() - lastFrame >= frameMs) {
    lastFrame = millis();
    render();
  }
  delay(2);
}
