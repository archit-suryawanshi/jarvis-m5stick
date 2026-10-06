// Jarvis's avatar. The robot is built each frame from simple shapes on a 48x56
// grid of palette indices, outlined automatically, then drawn at 2x. Its eye,
// antenna and chest colours follow the mood and blend smoothly between moods.

#include "avatar.h"

#include <math.h>
#include <string.h>

namespace {

constexpr int LW = 48, LH = 56, SCALE = 2;

enum : uint8_t {
  CLEAR = 0,
  OUTLINE,
  SHELL_LIGHT,
  SHELL_MID,
  SHELL_DARK,
  VISOR,
  VISOR_SHINE,
  GLOW_HI,  // mood colour, bright: eyes, antenna, chest light
  GLOW_MID,
  GLOW_LO,
  BLUSH,
  SHADOW,
  SHINE,
  HEART,
  COLOR_COUNT,
};

uint8_t fb[LW * LH];
int bobY = 0;  // offsets applied by put(), for bobbing and dancing
int bobX = 0;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Bright, mid and dark glow per mood, in Mood order.
const float MOOD_RGB[6][3][3] = {
  {{255, 168, 88}, {236, 118, 46}, {120, 58, 24}},     // Idle: Jarvis orange
  {{140, 232, 255}, {58, 170, 240}, {24, 80, 140}},    // Listening: cyan
  {{216, 164, 255}, {158, 100, 240}, {78, 44, 150}},   // Thinking: violet
  {{255, 118, 118}, {226, 58, 70}, {118, 24, 34}},     // Error: red
  {{206, 216, 255}, {146, 160, 228}, {68, 78, 138}},   // Sleepy: pale blue
  {{150, 248, 170}, {64, 206, 120}, {22, 104, 62}},    // Happy: green
};
float glow[3][3];
bool glowReady = false;

void put(int x, int y, uint8_t c)
{
  x += bobX;
  y += bobY;
  if (x >= 0 && x < LW && y >= 0 && y < LH) fb[y * LW + x] = c;
}

// Filled rectangle with rounded corners, inclusive bounds.
void rrect(int x0, int y0, int x1, int y1, int r, uint8_t c)
{
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      int dx = x < x0 + r ? x0 + r - x : (x > x1 - r ? x - (x1 - r) : 0);
      int dy = y < y0 + r ? y0 + r - y : (y > y1 - r ? y - (y1 - r) : 0);
      if (dx * dx + dy * dy <= r * r + r / 2) put(x, y, c);
    }
}

bool solid(uint8_t c)
{
  return c != CLEAR && c != OUTLINE && c != SHADOW && c != SHINE && c != HEART;
}

// Draws an outline on every empty pixel that touches the robot.
void outline()
{
  static uint8_t copy[LW * LH];
  memcpy(copy, fb, sizeof(fb));
  for (int y = 0; y < LH; y++)
    for (int x = 0; x < LW; x++) {
      if (copy[y * LW + x] != CLEAR) continue;
      auto s = [&](int xx, int yy) {
        return xx >= 0 && xx < LW && yy >= 0 && yy < LH && solid(copy[yy * LW + xx]);
      };
      if (s(x - 1, y) || s(x + 1, y) || s(x, y - 1) || s(x, y + 1)) fb[y * LW + x] = OUTLINE;
    }
}

// Paints only where nothing is drawn yet (shadow, sparkles).
void under(int x, int y, uint8_t c)
{
  if (x >= 0 && x < LW && y >= 0 && y < LH && fb[y * LW + x] == CLEAR) fb[y * LW + x] = c;
}

enum class Eyes { Open, Wide, Blink, Happy, Sleepy, Cross };

// One eye; (ex, ey) is the top-left of a 5x6 box.
void eye(int ex, int ey, Eyes shape)
{
  switch (shape) {
    case Eyes::Open:
      rrect(ex, ey, ex + 4, ey + 5, 1, GLOW_HI);
      put(ex + 1, ey + 1, SHINE);
      break;
    case Eyes::Wide:
      rrect(ex - 1, ey - 1, ex + 5, ey + 6, 2, GLOW_HI);
      put(ex, ey, SHINE);
      put(ex + 1, ey, SHINE);
      break;
    case Eyes::Blink:
      for (int i = 0; i <= 4; i++) put(ex + i, ey + 3, GLOW_HI);
      break;
    case Eyes::Happy:  // ^
      for (int i = 1; i <= 3; i++) put(ex + i, ey + 1, GLOW_HI);
      put(ex, ey + 2, GLOW_HI);
      put(ex + 4, ey + 2, GLOW_HI);
      put(ex, ey + 3, GLOW_HI);
      put(ex + 4, ey + 3, GLOW_HI);
      break;
    case Eyes::Sleepy:
      for (int i = 0; i <= 4; i++) put(ex + i, ey + 3, GLOW_MID);  // lid
      rrect(ex, ey + 4, ex + 4, ey + 5, 1, GLOW_HI);
      break;
    case Eyes::Cross:
      for (int i = 0; i <= 4; i++) {
        put(ex + i, ey + i, GLOW_HI);
        put(ex + 4 - i, ey + i, GLOW_HI);
      }
      break;
  }
}

void sparkle(int x, int y, float phase, uint8_t c)
{
  if (phase < 0.55f) return;
  under(x, y, SHINE);
  if (phase > 0.8f) {
    under(x - 1, y, c);
    under(x + 1, y, c);
    under(x, y - 1, c);
    under(x, y + 1, c);
  }
}

// A 5x4 heart, drawn behind the robot.
void heart(int x, int y)
{
  static const char *rows[] = {".X.X.", "XXXXX", ".XXX.", "..X.."};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 5; c++)
      if (rows[r][c] == 'X') under(x + c, y + r, HEART);
}

uint16_t mix565(const float *c, float k)
{
  return rgb((uint8_t)fminf(255, c[0] * k), (uint8_t)fminf(255, c[1] * k), (uint8_t)fminf(255, c[2] * k));
}

}  // namespace

void drawAvatar(M5Canvas &canvas, int ox, int oy, Mood mood, float level)
{
  const uint32_t now = millis();
  const float t = now / 1000.0f;
  const int m = (int)mood;

  // Blend the glow colours toward the mood's.
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      float target = MOOD_RGB[m][i][j];
      glow[i][j] = glowReady ? glow[i][j] + (target - glow[i][j]) * 0.18f : target;
    }
  glowReady = true;

  // Blinks every few seconds; idle glances to one side now and then.
  static uint32_t nextBlink = 0, blinkUntil = 0, nextGlance = 0, glanceUntil = 0;
  static int glanceDir = 1;
  if (now >= nextBlink) {
    blinkUntil = now + 130;
    nextBlink = now + 2500 + esp_random() % 3000;
  }
  if (now >= nextGlance) {
    glanceUntil = now + 900;
    glanceDir = (esp_random() & 1) ? 1 : -1;
    nextGlance = now + 4000 + esp_random() % 4000;
  }
  bool blinking = now < blinkUntil;

  memset(fb, CLEAR, sizeof(fb));

  // Pose for the mood
  int gx = 0, gy = 0;  // gaze
  Eyes eyes = Eyes::Open;
  int armLift = 0;
  int leftArmUp = 0, rightArmUp = 0;  // dance: raise one arm or the other
  float antenna = 0.5f + 0.5f * sinf(t * 2.0f);  // 0..1 brightness
  float chest = 0.5f + 0.5f * sinf(t * 2.0f + 1.5f);
  bobY = 0;
  bobX = 0;
  switch (mood) {
    case Mood::Idle:
      bobY = sinf(t * 2.4f) > 0.2f ? -1 : 0;
      if (now < glanceUntil) gx = 2 * glanceDir;
      eyes = blinking ? Eyes::Blink : Eyes::Open;
      break;
    case Mood::Listening:
      bobY = sinf(t * 6.0f) > 0.5f ? -1 : 0;
      eyes = blinking ? Eyes::Blink : Eyes::Wide;
      antenna = chest = fminf(1.0f, 0.3f + level * 1.2f);
      armLift = level > 0.35f ? 2 : 1;
      break;
    case Mood::Thinking: {
      int phase = (now / 700) % 4;  // look up-right, up, up-left, up
      gx = phase == 0 ? 2 : phase == 2 ? -2 : 0;
      gy = -1;
      eyes = blinking ? Eyes::Blink : Eyes::Open;
      antenna = (now / 250) % 2 ? 1.0f : 0.25f;
      chest = 0.5f + 0.5f * sinf(t * 8.0f);
      break;
    }
    case Mood::Error:
      eyes = Eyes::Cross;
      antenna = chest = 0.4f;
      break;
    case Mood::Sleepy:
      bobY = sinf(t * 1.2f) > 0.6f ? -1 : 0;
      eyes = Eyes::Sleepy;
      antenna = 0.3f + 0.7f * (0.5f + 0.5f * sinf(t * 1.5f));
      break;
    case Mood::Happy: {
      // Four beats: hop left with the left arm up, land, hop right with the
      // right arm up, land.
      int beat = (now / 200) % 4;
      bobY = beat % 2 == 0 ? -2 : 0;
      bobX = beat < 2 ? -1 : 1;
      leftArmUp = beat == 0 || beat == 1;
      rightArmUp = !leftArmUp;
      eyes = Eyes::Happy;
      antenna = (now / 100) % 2 ? 1.0f : 0.6f;
      chest = 1.0f;
      break;
    }
  }

  // Ears, with a light panel that glows while listening
  rrect(2, 15, 6, 26, 2, SHELL_MID);
  rrect(41, 15, 45, 26, 2, SHELL_MID);
  uint8_t earPanel = mood == Mood::Listening && level > 0.2f ? GLOW_MID : SHELL_DARK;
  rrect(3, 18, 4, 23, 0, earPanel);
  rrect(43, 18, 44, 23, 0, earPanel);

  // Antenna
  for (int y = 4; y <= 7; y++) {
    put(23, y, SHELL_DARK);
    put(24, y, SHELL_DARK);
  }
  rrect(21, 0, 26, 4, 2, antenna > 0.5f ? GLOW_HI : GLOW_MID);
  put(22, 1, SHINE);

  // Head: a darker shell with a lighter one offset up-left, for shading
  rrect(6, 7, 41, 33, 7, SHELL_MID);
  rrect(6, 7, 40, 31, 7, SHELL_LIGHT);
  put(11, 10, SHINE);
  put(12, 10, SHINE);
  put(11, 11, SHINE);

  // Visor face
  rrect(10, 12, 37, 28, 5, VISOR);
  put(13, 14, VISOR_SHINE);
  put(14, 14, VISOR_SHINE);
  put(13, 15, VISOR_SHINE);

  eye(15 + gx, 17 + gy, eyes);
  eye(28 + gx, 17 + gy, eyes);

  if (mood == Mood::Idle || mood == Mood::Listening || mood == Mood::Happy) {
    put(13, 24, BLUSH);
    put(14, 24, BLUSH);
    put(33, 24, BLUSH);
    put(34, 24, BLUSH);
  }

  // Mouth
  switch (mood) {
    case Mood::Idle:
      put(21, 25, GLOW_MID);
      put(26, 25, GLOW_MID);
      for (int x = 22; x <= 25; x++) put(x, 26, GLOW_MID);
      break;
    case Mood::Listening:
      if (level > 0.3f) rrect(22, 25, 25, 26, 1, GLOW_MID);
      else rrect(23, 25, 24, 26, 0, GLOW_MID);
      break;
    case Mood::Thinking:
      for (int x = 22 + gx / 2; x <= 25 + gx / 2; x++) put(x, 26, GLOW_MID);
      break;
    case Mood::Error:
      for (int x = 22; x <= 25; x++) put(x, 25, GLOW_MID);
      put(21, 26, GLOW_MID);
      put(26, 26, GLOW_MID);
      break;
    case Mood::Sleepy:
      put(23, 26, GLOW_MID);
      put(24, 26, GLOW_MID);
      break;
    case Mood::Happy:  // big open grin
      for (int x = 21; x <= 26; x++) put(x, 25, GLOW_MID);
      put(22, 26, GLOW_MID);
      put(23, 26, BLUSH);
      put(24, 26, BLUSH);
      put(25, 26, GLOW_MID);
      break;
  }

  // Neck, body and chest light
  rrect(19, 33, 28, 35, 0, SHELL_DARK);
  rrect(12, 34, 35, 48, 6, SHELL_MID);
  rrect(12, 34, 34, 46, 6, SHELL_LIGHT);
  rrect(20, 38, 27, 43, 2, VISOR);
  rrect(23, 40, 24, 41, 0, chest > 0.5f ? GLOW_HI : GLOW_LO);

  // Arms (raised a little while listening) and feet
  int sway = mood == Mood::Idle && sinf(t * 1.3f) > 0.7f ? 1 : 0;
  if (leftArmUp) rrect(7, 26, 10, 35, 2, SHELL_MID);  // waving above the shoulder
  else rrect(8, 36 - armLift, 11, 45 - armLift - sway, 2, SHELL_MID);
  if (rightArmUp) rrect(37, 26, 40, 35, 2, SHELL_MID);
  else rrect(36, 36 - armLift - sway, 39, 45 - armLift, 2, SHELL_MID);
  rrect(15, 48, 21, 51, 1, SHELL_MID);
  rrect(26, 48, 32, 51, 1, SHELL_MID);

  outline();
  bobY = 0;
  bobX = 0;

  // Ground shadow: narrower when bobbing up
  int rx = 11;
  for (int x = 24 - rx; x <= 23 + rx; x++) {
    under(x, 53, SHADOW);
    if (x > 24 - rx + 2 && x < 23 + rx - 2) under(x, 54, SHADOW);
  }

  // Hearts float up while dancing
  if (mood == Mood::Happy) {
    int rise = (now / 45) % 18;
    heart(0, 16 - rise);
    heart(43, 22 - (rise + 9) % 18);
  }

  // Sparkles while idle, thinking or dancing
  if (mood == Mood::Idle || mood == Mood::Thinking || mood == Mood::Happy) {
    sparkle(3, 6, 0.5f + 0.5f * sinf(t * 2.1f), GLOW_MID);
    sparkle(44, 10, 0.5f + 0.5f * sinf(t * 1.7f + 2.0f), GLOW_MID);
    sparkle(43, 44, 0.5f + 0.5f * sinf(t * 2.6f + 4.0f), GLOW_MID);
    sparkle(2, 38, 0.5f + 0.5f * sinf(t * 1.9f + 1.0f), GLOW_MID);
  }

  // Palette for this frame
  uint16_t pal[COLOR_COUNT];
  pal[OUTLINE] = rgb(26, 24, 36);
  pal[SHELL_LIGHT] = rgb(238, 234, 228);
  pal[SHELL_MID] = rgb(198, 192, 186);
  pal[SHELL_DARK] = rgb(140, 134, 132);
  pal[VISOR] = rgb(16, 20, 34);
  pal[VISOR_SHINE] = rgb(52, 62, 88);
  pal[GLOW_HI] = mix565(glow[0], mood == Mood::Listening ? 0.75f + 0.25f * antenna : 1.0f);
  pal[GLOW_MID] = mix565(glow[1], 1.0f);
  pal[GLOW_LO] = mix565(glow[2], 1.0f);
  pal[BLUSH] = rgb(255, 128, 140);
  pal[SHADOW] = rgb(30, 32, 44);
  pal[SHINE] = rgb(255, 255, 255);
  pal[HEART] = rgb(255, 92, 146);

  for (int y = 0; y < LH; y++)
    for (int x = 0; x < LW; x++) {
      uint8_t c = fb[y * LW + x];
      if (c != CLEAR) canvas.fillRect(ox + x * SCALE, oy + y * SCALE, SCALE, SCALE, pal[c]);
    }
}
