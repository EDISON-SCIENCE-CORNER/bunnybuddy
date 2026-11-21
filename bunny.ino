/*
  BUNNY FACE DISPLAY
  GC9A01A 1.28" round TFT (240x240) + ESP32-C3 Mini (C3 SuperMini / LOLIN C3 Mini)
  Made to sit inside a 3D-printed bunny head (the ears are on the print,
  the screen shows the face: eyes, nose, whiskers, mouth, blush).
  Library: "GFX Library for Arduino" (Arduino_GFX) by moononournation

  WIRING
    VCC->3V3  GND->GND  SCL->GPIO4  SDA->GPIO6  CS->GPIO10  DC->GPIO5  RST->GPIO1  BLK->GPIO3
    Face button (6x6 tactile in the bunny's head): one leg -> GPIO0, the other leg -> GND
      (uses the internal pull-up, no resistor needed)
    (GPIO2, 8 and 9 are boot-strapping pins and GPIO7 is the RGB LED on the LOLIN C3 Mini,
     so they are kept free.)

  MODES  (hold the button ~1 s and release to go to the next mode)
    FACES -> TIMER -> STOPWATCH -> CLOCK -> COLOUR -> FACES ...

  CONTROLS (face button on GPIO0, or the BOOT button)
    FACES : tap = next face          hold 3 s = AUTO mode on/off (green/grey ring)
    TIMER : tap = 1 / 3 / 5 / 10 min, starts by itself 3 s after the last tap
            while running: tap = pause/resume, hold = cancel
            when done: bunny celebrates, tap to go back to the faces
    STOPWATCH : tap = start / stop, hold 3 s while stopped = reset to zero
    CLOCK : tap = switch analog / digital
    COLOUR: tap = next colour theme (pink, white, lilac, mint, peach, sky,
            black+white, white+black) - saved, used everywhere
    Serial Monitor (115200): type a number 0..16 to show that expression.

  WIFI / WEATHER
    Fill in WIFI_SSID, WIFI_PASS, OWM_KEY and OWM_CITY below.
    Time comes from the internet (NTP), weather from OpenWeatherMap (free key).

  Board: "ESP32C3 Dev Module" (or "LOLIN C3 Mini"), USB CDC On Boot: Enabled
*/

#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_system.h>

enum Mode { M_FACES, M_TIMER_SET, M_TIMER_RUN, M_TIMER_DONE, M_STOPWATCH, M_CLOCK, M_COLOUR };
enum Ev   { EV_NONE, EV_SHORT, EV_LONG, EV_VLONG };

// ---------- WiFi / clock / weather settings ----------
const char *WIFI_SSID = "edison science corner";
const char *WIFI_PASS = "eeeeeeee";
const char *TZ_INFO   = "IST-5:30";                        // India Standard Time
const char *OWM_KEY   = "************************";     // from openweathermap.org (free)
const char *OWM_CITY  = "IDUKKI,IN";                    // e.g. "Kochi,IN"
const bool  CLOCK_24H = false;                             // false = 12-hour clock
const unsigned long WEATHER_EVERY_MS = 10UL * 60 * 1000;   // refresh weather every 10 min

// ---------- Pins ----------
#define TFT_SCK    4
#define TFT_MOSI   6
#define TFT_CS    10
#define TFT_DC     5
#define TFT_RST    1
#define TFT_BL     3
#define BTN_PIN    9     // on-board BOOT button (active LOW)
#define FACE_BTN   0     // 6x6 tactile button to GND (active LOW)

// ---------- Settings ----------
const unsigned long AUTO_INTERVAL_MS = 7000;   // time per expression in auto mode
const unsigned long TIMER_START_DELAY = 3000;  // timer starts this long after the last tap
const uint8_t TIMER_OPTS[] = {1, 3, 5, 10};     // timer choices in minutes
const uint8_t BL_NORMAL = 255, BL_SLEEPY = 70; // backlight dims while sleepy

// ---------- Display (full-frame canvas = flicker-free) ----------
Arduino_DataBus *bus = new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *panel   = new Arduino_GC9A01(bus, TFT_RST, 0, true);
// The canvas skips the top 2 and bottom 2 pixel rows (outside the round glass anyway):
// 240x236 fits in the C3's largest free RAM block once WiFi is linked in.
const int CANVAS_H = 236, CANVAS_Y = 2;
const int CY0 = 120 - CANVAS_Y;                  // screen centre in canvas coordinates
Arduino_Canvas *cv   = new Arduino_Canvas(240, CANVAS_H, panel, 0, CANVAS_Y);

// ---------- Colours ----------
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// Colours are set by applyTheme() below (change them in the COLOUR menu).
uint16_t C_FACE     = rgb(242, 150, 190);
uint16_t C_INK      = rgb(30, 25, 35);
uint16_t C_WHITE    = rgb(255, 255, 255);
uint16_t C_NOSE     = rgb(190, 30, 90);
uint16_t C_BLUSH    = rgb(250, 100, 145);
uint16_t C_DOTS     = rgb(255, 255, 255);
uint16_t C_MOUTH    = rgb(120, 25, 45);
uint16_t C_TONGUE   = rgb(255, 185, 205);
uint16_t C_HEART    = rgb(215, 15, 65);
uint16_t C_TEAR     = rgb(90, 180, 255);
uint16_t C_BUBBLE   = rgb(185, 225, 255);
uint16_t C_GOLD     = rgb(255, 180, 0);
uint16_t C_ANGRY    = rgb(255, 70, 70);
uint16_t C_CARROT   = rgb(255, 130, 20);
uint16_t C_CARROT_D = rgb(210, 90, 0);
uint16_t C_LEAF     = rgb(60, 180, 70);
uint16_t C_GREEN    = rgb(0, 200, 90);
uint16_t C_GREY     = rgb(150, 150, 160);
uint16_t C_CLOUD    = rgb(255, 255, 255);
uint16_t C_TRACK    = 0;

// ---------- Colour themes ----------
const char *THEME_NAMES[] = {"PINK", "WHITE", "LILAC", "MINT", "PEACH", "SKY", "BLACK+WHITE", "WHITE+BLACK"};
const int NUM_THEMES = 8;
int theme = 0;

uint16_t lerp565(uint16_t a, uint16_t b, float k);

void applyTheme(int t) {
  theme = (t % NUM_THEMES + NUM_THEMES) % NUM_THEMES;
  // colourful defaults
  C_INK = rgb(30, 25, 35);     C_WHITE = rgb(255, 255, 255); C_NOSE = rgb(190, 30, 90);
  C_BLUSH = rgb(250, 100, 145); C_DOTS = rgb(255, 255, 255); C_MOUTH = rgb(120, 25, 45);
  C_TONGUE = rgb(255, 185, 205); C_HEART = rgb(215, 15, 65); C_TEAR = rgb(90, 180, 255);
  C_BUBBLE = rgb(185, 225, 255); C_GOLD = rgb(255, 180, 0);  C_ANGRY = rgb(255, 70, 70);
  C_CARROT = rgb(255, 130, 20); C_CARROT_D = rgb(210, 90, 0); C_LEAF = rgb(60, 180, 70);
  C_GREEN = rgb(0, 200, 90);   C_GREY = rgb(150, 150, 160);  C_CLOUD = rgb(255, 255, 255);
  switch (theme) {
    case 0: C_FACE = rgb(242, 150, 190); break;                                        // pink
    case 1: C_FACE = rgb(252, 248, 252); C_DOTS = rgb(250, 150, 185); C_CLOUD = rgb(190, 195, 210); break;
    case 2: C_FACE = rgb(225, 210, 240); break;                                        // lilac
    case 3: C_FACE = rgb(185, 235, 210); break;                                        // mint
    case 4: C_FACE = rgb(255, 205, 170); break;                                        // peach
    case 5: C_FACE = rgb(180, 215, 250); C_BUBBLE = rgb(235, 245, 255); break;         // sky
    case 6:                                                                            // black + white
      C_FACE = rgb(0, 0, 0);       C_INK = rgb(255, 255, 255);  C_WHITE = rgb(0, 0, 0);
      C_NOSE = C_INK; C_BLUSH = rgb(70, 70, 70); C_DOTS = C_INK; C_MOUTH = C_INK; C_TONGUE = C_FACE;
      C_HEART = C_INK; C_TEAR = C_INK; C_BUBBLE = rgb(120, 120, 120); C_GOLD = C_INK; C_ANGRY = C_INK;
      C_CARROT = C_INK; C_CARROT_D = C_FACE; C_LEAF = rgb(150, 150, 150); C_GREEN = C_INK;
      C_GREY = rgb(110, 110, 110); C_CLOUD = rgb(170, 170, 170);
      break;
    case 7:                                                                            // white + black
      C_FACE = rgb(255, 255, 255); C_INK = rgb(0, 0, 0);        C_WHITE = rgb(255, 255, 255);
      C_NOSE = C_INK; C_BLUSH = rgb(205, 205, 205); C_DOTS = C_INK; C_MOUTH = C_INK; C_TONGUE = C_FACE;
      C_HEART = C_INK; C_TEAR = C_INK; C_BUBBLE = rgb(170, 170, 170); C_GOLD = C_INK; C_ANGRY = C_INK;
      C_CARROT = C_INK; C_CARROT_D = C_FACE; C_LEAF = rgb(120, 120, 120); C_GREEN = C_INK;
      C_GREY = rgb(140, 140, 140); C_CLOUD = rgb(150, 150, 150);
      break;
  }
  C_TRACK = lerp565(C_FACE, C_INK, 0.12f);
}

uint16_t lerp565(uint16_t a, uint16_t b, float k) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * k) << 11) | ((int)(ag + (bg - ag) * k) << 5) | (int)(ab + (bb - ab) * k);
}

// ---------- Face coordinate system ----------
float S = 1.0f, CXf = 120, CYf = CY0;   // feature scale + centre (for pop / bounce / shake)
float blinkAmt = 0;                      // 0 open .. 1 closed
float lookX = 0, lookY = 0;              // eye gaze offset
float twitch = 0;                        // bunny nose twitch offset

int X(float d) { return (int)lroundf(CXf + d * S); }
int Y(float d) { return (int)lroundf(CYf + d * S); }
int L(float d) { int v = (int)lroundf(d * S); return v < 1 ? 1 : v; }

// Feature positions
const float EX = 50, EY = -14;     // eyes at (+-EX, EY)
const float NY = 22;               // nose y
const float MY = 34;               // mouth y

// ---------- Drawing helpers ----------
void thick(float x0, float y0, float x1, float y1, float th, uint16_t c = C_INK) {
  float ax = X(x0), ay = Y(y0), bx = X(x1), by = Y(y1);
  float r = th * S / 2; if (r < 1) r = 1;
  float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
  if (len > 0.5f) {
    float nx = -dy / len * r, ny = dx / len * r;
    cv->fillTriangle(ax + nx, ay + ny, ax - nx, ay - ny, bx + nx, by + ny, c);
    cv->fillTriangle(ax - nx, ay - ny, bx - nx, by - ny, bx + nx, by + ny, c);
  }
  cv->fillCircle(ax, ay, r, c);
  cv->fillCircle(bx, by, r, c);
}

// Arc in degrees: 0 = 3 o'clock, clockwise. th >= r gives a filled pie.
void arcR(float dx, float dy, float r, float th, float a0, float a1, uint16_t c = C_INK) {
  if (a1 > 360) { arcR(dx, dy, r, th, a0, 360, c); arcR(dx, dy, r, th, 0, a1 - 360, c); return; }
  int inner = (int)lroundf((r - th) * S); if (inner < 0) inner = 0;
  cv->fillArc(X(dx), Y(dy), L(r), inner, a0, a1, c);
}

void heart(float dx, float dy, float sz, uint16_t c = C_HEART) {
  float r = sz * 0.27f;
  cv->fillCircle(X(dx - r), Y(dy - r * 0.4f), L(r), c);
  cv->fillCircle(X(dx + r), Y(dy - r * 0.4f), L(r), c);
  cv->fillTriangle(X(dx - r * 1.95f), Y(dy - r * 0.1f), X(dx + r * 1.95f), Y(dy - r * 0.1f), X(dx), Y(dy + sz * 0.6f), c);
}

void star(float dx, float dy, float R, float rot, uint16_t c = C_GOLD) {
  float ri = R * 0.45f;
  for (int i = 0; i < 5; i++) {
    float a0 = rot + i * 1.2566f - 1.5708f, a1 = a0 + 0.6283f, a2 = a0 + 1.2566f;
    cv->fillTriangle(X(dx), Y(dy), X(dx + cosf(a0) * R), Y(dy + sinf(a0) * R), X(dx + cosf(a1) * ri), Y(dy + sinf(a1) * ri), c);
    cv->fillTriangle(X(dx), Y(dy), X(dx + cosf(a1) * ri), Y(dy + sinf(a1) * ri), X(dx + cosf(a2) * R), Y(dy + sinf(a2) * R), c);
  }
}

void twinkle(float x, float y, float s, uint16_t c = C_GOLD) {
  if (s < 1) return;
  float w = s * 0.28f;
  cv->fillTriangle(X(x), Y(y - s), X(x + w), Y(y), X(x - w), Y(y), c);
  cv->fillTriangle(X(x), Y(y + s), X(x + w), Y(y), X(x - w), Y(y), c);
  cv->fillTriangle(X(x - s), Y(y), X(x), Y(y - w), X(x), Y(y + w), c);
  cv->fillTriangle(X(x + s), Y(y), X(x), Y(y - w), X(x), Y(y + w), c);
}

void tear(float dx, float dy, float r, uint16_t c = C_TEAR) {
  cv->fillCircle(X(dx), Y(dy), L(r), c);
  cv->fillTriangle(X(dx - r * 0.92f), Y(dy - r * 0.4f), X(dx + r * 0.92f), Y(dy - r * 0.4f), X(dx), Y(dy - r * 2.3f), c);
  cv->fillCircle(X(dx - r * 0.35f), Y(dy - r * 0.1f), L(r * 0.25f), C_WHITE);
}

void spiral(float dx, float dy, float R, float rot) {
  for (float a = 0; a < 12.5f; a += 0.22f) {
    float r = R * a / 12.5f;
    cv->fillCircle(X(dx + cosf(a + rot) * r), Y(dy + sinf(a + rot) * r), L(2.4f), C_INK);
  }
}

void vein(float x, float y, float s) {
  arcR(x - s, y - s, s, s * 0.4f, 0, 90, C_ANGRY);
  arcR(x + s, y - s, s, s * 0.4f, 90, 180, C_ANGRY);
  arcR(x + s, y + s, s, s * 0.4f, 180, 270, C_ANGRY);
  arcR(x - s, y + s, s, s * 0.4f, 270, 360, C_ANGRY);
}

// ----- Bunny parts -----
// side = -1 for the left eye, +1 for the right eye (lashes go to the outside)

void lashesOpen(float ex, float ey, float rx, float h, float side) {
  float x = ex + side * rx * 0.72f, y = ey - h * 0.68f;
  thick(x, y, x + side * 10, y - 7, 3);
  thick(x + side * 4, y + 6, x + side * 13, y + 2, 3);
}

// Big glossy eye with lashes; blinks automatically
void bigEye(float dx, float dy, float side, float rx = 20, float ry = 23, bool lashes = true) {
  float h = ry * (1.0f - blinkAmt);
  if (h < 5) {                                     // mid-blink: show closed lash eye
    arcR(dx, dy - 10, 18, 5, 20, 160);
    return;
  }
  float ex = dx + lookX, ey = dy + lookY;
  cv->fillEllipse(X(ex), Y(ey), L(rx), L(h), C_INK);
  if (h > ry * 0.5f) {
    cv->fillCircle(X(ex + rx * 0.3f), Y(ey - h * 0.35f), L(rx * 0.38f), C_WHITE);
    cv->fillCircle(X(ex - rx * 0.4f), Y(ey + h * 0.42f), L(rx * 0.17f), C_WHITE);
  }
  if (lashes) lashesOpen(ex, ey, rx, h, side);
}

// Happy closed eye  ∩  with lashes at the outer end
void happyEye(float dx, float dy, float side) {
  arcR(dx, dy + 8, 18, 5, 200, 340);
  float px = dx + side * 16.9f, py = dy + 8 - 6.2f;
  thick(px, py, px + side * 8, py + 2, 3);
  thick(px - side * 2, py - 4, px + side * 6, py - 9, 3);
}

// Relaxed closed eye  ∪  with lashes underneath (sleepy / shy)
void sleepEye(float dx, float dy) {
  arcR(dx, dy - 10, 18, 5, 20, 160);
  for (int a = 55; a <= 125; a += 35) {
    float c = cosf(a * DEG_TO_RAD), s = sinf(a * DEG_TO_RAD);
    thick(dx + c * 19, dy - 10 + s * 19, dx + c * 27, dy - 10 + s * 27, 3);
  }
}

// Squeezed eye  > <
void squeezeEye(float dx, float dy, float side) {
  float o = -side * 12;   // point toward the nose
  thick(dx + side * 14, dy - 12, dx + o, dy, 6);
  thick(dx + o, dy, dx + side * 14, dy + 12, 6);
}

void nose(float extraY = 0) {
  heart(0, NY + twitch + extraY, 15, C_NOSE);
  cv->fillCircle(X(-3), Y(NY - 3 + twitch + extraY), L(1.8f), C_WHITE);
}

// ω mouth
void mouthW(float y = MY) {
  thick(0, NY + 6 + twitch, 0, y, 2.5f);
  arcR(-7, y, 7, 2.5f, 0, 180);
  arcR(7, y, 7, 2.5f, 0, 180);
}

void mouthOpen(float r, float y = MY) {
  thick(0, NY + 6 + twitch, 0, y, 2.5f);
  arcR(0, y, r, r, 0, 180, C_MOUTH);
  cv->fillEllipse(X(0), Y(y + r * 0.62f), L(r * 0.5f), L(r * 0.28f), C_TONGUE);
}

void mouthFrown(float y = MY + 8) { arcR(0, y + 10, 12, 3, 210, 330); }

void whiskers(float spread = 0, float wig = 0) {
  for (int i = -1; i <= 1; i++) {
    float y0 = NY + 6 + i * 8, y1 = NY + 2 + i * (16 + spread) + wig + twitch;
    thick(-62, y0, -104, y1, 2.5f);
    thick(62, y0, 104, y1, 2.5f);
  }
}

void blush(float k = 1, uint16_t c = C_BLUSH) {
  cv->fillEllipse(X(-74), Y(14), L(17 * k), L(10 * k), c);
  cv->fillEllipse(X(74),  Y(14), L(17 * k), L(10 * k), c);
}

void foreheadDots() {
  const float d[5][2] = {{0, -60}, {-7, -70}, {7, -70}, {-4, -81}, {4, -81}};
  for (auto &p : d) cv->fillCircle(X(p[0]), Y(p[1]), L(3), C_DOTS);
}

// Standard bunny base: forehead dots, blush, whiskers, nose
void base(float blushK = 1, uint16_t blushC = C_BLUSH, float spread = 0, float wig = 0) {
  foreheadDots();
  blush(blushK, blushC);
  whiskers(spread, wig);
  nose();
}

void brows(float tilt, float y = -50) {   // tilt>0 = angry (inner low), tilt<0 = sad (inner high)
  thick(-EX - 18, y - tilt, -EX + 16, y + tilt, 4);
  thick(EX + 18, y - tilt, EX - 16, y + tilt, 4);
}

// ---------- Expressions ----------
void e_smile(float t) {
  base();
  bigEye(-EX, EY, -1); bigEye(EX, EY, 1);
  mouthW();
}

void e_happy(float t) {
  CYf += sinf(t * 7) * 3;
  base(1.2f);
  happyEye(-EX, EY, -1); happyEye(EX, EY, 1);
  mouthOpen(12);
}

void e_laugh(float t) {
  CXf += sinf(t * 45) * 2; CYf += sinf(t * 9) * 3;
  base(1.2f, C_BLUSH, 4, sinf(t * 20) * 2);
  squeezeEye(-EX, EY, -1); squeezeEye(EX, EY, 1);
  mouthOpen(17);
  for (int k = 0; k < 2; k++) {
    float p = fmodf(t * 1.6f + k * 0.5f, 1.0f);
    tear(-(68 + p * 30), -26 + p * p * 60, 8 - p * 3);
    tear( (68 + p * 30), -26 + p * p * 60, 8 - p * 3);
  }
}

void e_love(float t) {
  base(1.2f);
  float pl = 1 + 0.12f * sinf(t * 9);
  for (int s = -1; s <= 1; s += 2) {
    heart(s * EX, EY, 46 * pl);
    cv->fillCircle(X(s * EX + 7 * pl), Y(EY - 9 * pl), L(4 * pl), C_WHITE);
  }
  mouthW();
  for (int k = 0; k < 2; k++) {
    float p = fmodf(t * 0.35f + k * 0.5f, 1.0f);
    float side = k ? 1 : -1;
    heart(side * 85 + sinf(t * 3 + k) * 6, 50 - p * 150, 16 * (1 - p * 0.4f));
  }
}

void e_kiss(float t) {
  base(1.3f);
  happyEye(-EX, EY, -1); happyEye(EX, EY, 1);
  arcR(3, MY - 2, 6, 3, 270, 450); arcR(3, MY + 10, 6, 3, 270, 450);   // "3" lips
  float p = fmodf(t * 0.6f, 1.0f);
  heart(22 + p * 60, MY - p * 110, 12 + p * 16);
}

void e_wink(float t) {
  base();
  bigEye(-EX, EY, -1);
  happyEye(EX, EY, 1);
  float w = sinf(t * 5) * 2;
  cv->fillEllipse(X(w), Y(MY + 10), L(8), L(10), C_TONGUE);
  thick(w, MY + 4, w, MY + 14, 2, C_HEART);
  mouthW();
}

void e_surprised(float t) {
  base(0.8f, C_BLUSH, -6);
  arcR(-EX, EY - 32, 16, 4, 220, 320); arcR(EX, EY - 32, 16, 4, 220, 320);
  bigEye(-EX, EY, -1, 22, 27); bigEye(EX, EY, 1, 22, 27);
  float o = sinf(t * 4) * 2;
  thick(0, NY + 6 + twitch, 0, MY - 2, 2.5f);
  cv->fillEllipse(X(0), Y(MY + 8), L(8 + o), L(10 + o), C_MOUTH);
}

void e_starstruck(float t) {
  base(1.2f);
  float pr = 1 + 0.1f * sinf(t * 8);
  star(-EX, EY, 26 * pr, t * 1.5f); star(EX, EY, 26 * pr, -t * 1.5f);
  mouthOpen(13);
  twinkle(-85, -55, 12 * fabsf(sinf(t * 3)));
  twinkle(85, -55, 12 * fabsf(sinf(t * 3 + 2)));
  twinkle(-30, -90, 9 * fabsf(sinf(t * 3 + 4)));
  twinkle(30, -90, 9 * fabsf(sinf(t * 3 + 1)));
}

void e_sleepy(float t) {
  CYf += sinf(t * 1.2f) * 3;
  base(0.9f);
  sleepEye(-EX, EY + 6); sleepEye(EX, EY + 6);
  mouthW();
  // nose bubble grows and pops
  float p = fmodf(t * 0.3f, 1.0f);
  if (p < 0.9f) {
    float r = 4 + p * 16;
    cv->fillCircle(X(14 + r * 0.7f), Y(NY + 2 - r * 0.2f), L(r), C_BUBBLE);
    cv->fillCircle(X(14 + r * 0.7f + r * 0.35f), Y(NY + 2 - r * 0.55f), L(r * 0.22f), C_WHITE);
  }
  cv->setTextColor(C_INK);
  for (int k = 0; k < 3; k++) {
    float q = fmodf(t * 0.35f + k / 3.0f, 1.0f);
    int sz = (int)((1.5f + q * 2.5f) * S); if (sz < 1) sz = 1;
    cv->setTextSize(sz);
    cv->setCursor(X(40 + q * 35), Y(-55 - q * 40));
    cv->print('Z');
  }
}

void e_shy(float t) {
  lookX = 0; lookY = 0;
  CYf += sinf(t * 2) * 2;
  base(1.35f + 0.15f * sinf(t * 4));
  sleepEye(-EX, EY + 6); sleepEye(EX, EY + 6);
  mouthW(MY - 2);
  float p = fmodf(t * 0.5f, 1.0f);
  heart(80 + sinf(t * 4) * 4, -40 - p * 45, 13 * (1 - p * 0.5f));
}

void e_sad(float t) {
  lookX = 0; lookY = 4;
  base(0.8f, C_BLUSH, 6);
  brows(-5, -48);
  bigEye(-EX, EY, -1, 20, 22); bigEye(EX, EY, 1, 20, 22);
  // extra watery shine
  cv->fillCircle(X(-EX - 6), Y(EY + 10), L(4), C_WHITE);
  cv->fillCircle(X(EX - 6), Y(EY + 10), L(4), C_WHITE);
  mouthFrown();
  float p = fmodf(t * 0.45f, 1.0f);
  tear(-EX - 10, EY + 18 + p * 70, 7);
}

void e_cry(float t) {
  CYf += sinf(t * 12) * 1.5f;
  base(1.0f, C_BLUSH, 6);
  thick(-EX + 6, EY + 6, -EX + 2, 125, 11, C_TEAR); thick(EX - 6, EY + 6, EX - 2, 125, 11, C_TEAR);
  for (int k = 0; k < 2; k++) {
    float p = fmodf(t * 1.2f + k * 0.5f, 1.0f);
    cv->fillCircle(X(-EX + 4), Y(EY + 10 + p * 100), L(7), C_TEAR);
    cv->fillCircle(X(EX - 4),  Y(EY + 10 + p * 100), L(7), C_TEAR);
  }
  brows(-5, -46);
  happyEye(-EX, EY + 4, -1); happyEye(EX, EY + 4, 1);
  float wy = sinf(t * 10) * 3;
  thick(0, NY + 6 + twitch, 0, MY, 2.5f);
  cv->fillEllipse(X(0), Y(MY + 10), L(16), L(11 + wy), C_MOUTH);
  cv->fillEllipse(X(0), Y(MY + 16 + wy * 0.6f), L(8), L(4), C_TONGUE);
}

void e_angry(float t) {
  float k = 0.5f + 0.5f * sinf(t * 4);
  CXf += sinf(t * 55) * 2;
  base(1.2f + 0.2f * k, lerp565(C_BLUSH, C_ANGRY, k), -4, sinf(t * 30) * 3);
  brows(8, -42);
  bigEye(-EX, EY + 4, -1, 16, 16, false); bigEye(EX, EY + 4, 1, 16, 16, false);
  mouthFrown();
  vein(75, -65, 9 * (1 + 0.2f * sinf(t * 10)));
}

void e_dizzy(float t) {
  CXf += sinf(t * 2.5f) * 6; CYf += cosf(t * 2.5f) * 4;
  base(1.0f, C_BLUSH, 0, sinf(t * 5) * 4);
  spiral(-EX, EY, 20, t * 6); spiral(EX, EY, 20, -t * 6);
  thick(0, NY + 6 + twitch, 0, MY, 2.5f);
  for (int i = 0; i < 7; i++) {
    float x0 = -21 + i * 6, x1 = x0 + 6;
    thick(x0, MY + 4 + sinf(x0 * 0.35f + t * 6) * 4, x1, MY + 4 + sinf(x1 * 0.35f + t * 6) * 4, 3);
  }
  for (int k = 0; k < 3; k++) {
    float a = t * 3 + k * 2.094f;
    star(cosf(a) * 50, -78 + sinf(a) * 10, 9, t * 4);
  }
}

void e_nervous(float t) {
  lookX = (fmodf(t, 1.2f) < 0.6f) ? -7 : 7; lookY = 0;
  base(1.0f, C_BLUSH, 3, sinf(t * 25) * 1.5f);
  brows(-4, -48);
  bigEye(-EX, EY, -1, 18, 21); bigEye(EX, EY, 1, 18, 21);
  thick(0, NY + 6 + twitch, 0, MY - 2, 2.5f);
  for (int i = 0; i < 6; i++) {     // wobbly zig-zag mouth
    float x0 = -18 + i * 6, x1 = x0 + 6;
    float j = sinf(t * 20) * 1.5f;
    thick(x0, MY + 4 + ((i & 1) ? -3 : 3) + j, x1, MY + 4 + ((i & 1) ? 3 : -3) + j, 2.5f);
  }
  float p = fmodf(t * 0.4f, 1.0f);
  tear(88, -50 + p * 40, 9);
}

void e_suspicious(float t) {
  lookX = sinf(t * 1.3f) * 9; lookY = 0;
  base(0.8f);
  bigEye(-EX, EY, -1, 20, 23, false); bigEye(EX, EY, 1, 20, 23, false);
  // heavy lids: cover the top of both eyes
  cv->fillRect(X(-EX - 32), Y(EY - 30), L(64), L(26), C_FACE);
  cv->fillRect(X(EX - 32),  Y(EY - 30), L(64), L(20), C_FACE);
  thick(-EX - 24, EY - 4, -EX + 24, EY - 4, 4);
  thick(EX - 24, EY - 10, EX + 24, EY - 10, 4);
  thick(-10, MY + 4, 12, MY, 3);
}

void e_carrot(float t) {   // munching a carrot
  base(1.2f + 0.15f * fabsf(sinf(t * 8)));
  happyEye(-EX, EY, -1); happyEye(EX, EY, 1);
  float p = fmodf(t * 0.25f, 1.0f);
  float len = 70 * (1 - p) + 8;               // carrot gets eaten
  float chew = fabsf(sinf(t * 8)) * 4;
  float mx = 6, my = MY + 6;
  // carrot (pointing down-right from the mouth)
  float ax = 0.82f, ay = 0.57f;               // direction
  float tx = mx + ax * len, ty = my + ay * len;
  float w = 9;
  cv->fillTriangle(X(mx - ay * w), Y(my + ax * w), X(mx + ay * w), Y(my - ax * w), X(tx), Y(ty), C_CARROT);
  for (int i = 1; i < 4; i++) {
    float q = len * i / 4.0f, ww = w * (1 - (float)i / 4) * 0.8f;
    thick(mx + ax * q - ay * ww, my + ay * q + ax * ww, mx + ax * q - ay * ww * 0.2f, my + ay * q + ax * ww * 0.2f, 2, C_CARROT_D);
  }
  if (p < 0.08f) {   // fresh carrot shows its leaves before the first bite
    thick(mx, my, mx - 14, my - 10, 4, C_LEAF);
    thick(mx, my, mx - 16, my + 2, 4, C_LEAF);
  }
  // chewing mouth over the carrot end
  cv->fillEllipse(X(0), Y(MY + 4), L(10), L(5 + chew), C_MOUTH);
  thick(0, NY + 6 + twitch, 0, MY - 1, 2.5f);
  // crumbs
  for (int k = 0; k < 3; k++) {
    float q = fmodf(t * 1.5f + k * 0.33f, 1.0f);
    cv->fillCircle(X(-10 + k * 9 + q * 6), Y(MY + 14 + q * 50), L(2.5f), C_CARROT);
  }
}

struct Expr { const char *name; void (*draw)(float); };
Expr EXPR[] = {
  {"Smile", e_smile},         {"Happy", e_happy},           {"Laugh", e_laugh},
  {"Love", e_love},           {"Kiss", e_kiss},             {"Wink", e_wink},
  {"Surprised", e_surprised}, {"Star-struck", e_starstruck}, {"Sleepy", e_sleepy},
  {"Shy", e_shy},             {"Sad", e_sad},               {"Cry", e_cry},
  {"Angry", e_angry},         {"Dizzy", e_dizzy},           {"Nervous", e_nervous},
  {"Suspicious", e_suspicious}, {"Carrot", e_carrot},
};
const int NUM_EXPR = sizeof(EXPR) / sizeof(EXPR[0]);
const int SLEEPY_INDEX = 8;

// ---------- State ----------
Mode mode = M_FACES;
Preferences prefs;
int  cur = 0;
bool autoMode = true, analogClock = true;
unsigned long switchedAt = 0, ringUntil = 0, modeAt = 0;
unsigned long nextBlink = 0, blinkStart = 0, nextLook = 0, nextTwitch = 0, twitchStart = 0;
float lookTX = 0, lookTY = 0;

// timer
int tmrSel = 1;                                   // index into TIMER_OPTS
unsigned long tmrTotal = 0, tmrEnd = 0, tmrLeftAtPause = 0, lastSetPress = 0;
bool tmrPaused = false;

// weather (filled in by a background task)
volatile bool wxValid = false;
volatile float wxTemp = 0;
char wxCode[4] = "";                                // OpenWeatherMap icon code, e.g. "10d"

// button
bool btnDown = false;
unsigned long holdMs = 0;

void setBacklight(uint8_t v) { analogWrite(TFT_BL, v); }

void setExpr(int i, bool save) {
  cur = (i % NUM_EXPR + NUM_EXPR) % NUM_EXPR;
  switchedAt = millis();
  lookX = lookY = lookTX = lookTY = 0;
  setBacklight(cur == SLEEPY_INDEX ? BL_SLEEPY : BL_NORMAL);
  Serial.printf("Expression %d: %s\n", cur, EXPR[cur].name);
  if (save) prefs.putUChar("expr", cur);
}

void setMode(Mode m) {
  mode = m;
  modeAt = millis();
  setBacklight(BL_NORMAL);
  if (m == M_FACES)     setExpr(cur, false);
  if (m == M_TIMER_SET) lastSetPress = millis();
  if (m == M_TIMER_DONE) { setExpr(7, false); setBacklight(BL_NORMAL); }   // Star-struck
  if (m == M_COLOUR) switchedAt = millis();                                // pop the preview face in
  const char *names[] = {"FACES", "TIMER (set)", "TIMER (running)", "TIMER (done)", "STOPWATCH", "CLOCK", "COLOUR"};
  Serial.printf("Mode: %s\n", names[m]);
}

// ---------- Button ----------

bool isPressed() {
  return digitalRead(BTN_PIN) == LOW || digitalRead(FACE_BTN) == LOW;
}

// short = tap, long = held 0.7..3 s then released, vlong = held 3 s (fires while held)
Ev readButton(unsigned long now) {
  static bool stable = false, last = false, vlongDone = false;
  static unsigned long changedAt = 0, pressAt = 0;
  bool raw = isPressed();
  if (raw != stable && now - changedAt > 30) { stable = raw; changedAt = now; }   // debounce
  Ev ev = EV_NONE;
  if (stable && !last) { pressAt = now; vlongDone = false; }
  if (stable && last && !vlongDone && now - pressAt > 3000) { vlongDone = true; ev = EV_VLONG; }
  if (!stable && last && !vlongDone) ev = (now - pressAt >= 700) ? EV_LONG : EV_SHORT;
  last = stable;
  btnDown = stable;
  holdMs = (stable && !vlongDone) ? now - pressAt : 0;
  return ev;
}

void handleSerial() {
  if (!Serial.available()) return;
  int c = Serial.peek();
  bool isNum = (c >= '0' && c <= '9');
  int n = isNum ? Serial.parseInt() : -1;   // ignore stray non-numeric input
  while (Serial.available()) Serial.read();
  if (isNum && n >= 0 && n < NUM_EXPR) { autoMode = false; cur = n; setMode(M_FACES); prefs.putUChar("expr", cur); }
}

// ---------- Life (blink / look / twitch) ----------
void updateLife(unsigned long now) {
  if (now > nextBlink) { blinkStart = now; nextBlink = now + random(2000, 5500); }
  float bp = (now - blinkStart) / 160.0f;
  blinkAmt = bp < 1 ? 1 - fabsf(bp * 2 - 1) : 0;

  if (now > nextLook) {
    nextLook = now + random(700, 3000);
    if (random(10) < 4) { lookTX = 0; lookTY = 0; }
    else { lookTX = random(-8, 9); lookTY = random(-4, 5); }
  }
  lookX += (lookTX - lookX) * 0.25f;
  lookY += (lookTY - lookY) * 0.25f;

  if (now > nextTwitch) { twitchStart = now; nextTwitch = now + random(2500, 6000); }
  float tp = (now - twitchStart) / 450.0f;
  twitch = tp < 1 ? -fabsf(sinf(tp * 3 * PI)) * 3 : 0;
}

// ---------- Screen helpers (absolute pixel coordinates) ----------

// ring arc, degrees: 0 = 3 o'clock, clockwise; 270 = 12 o'clock
void ring(float a0, float a1, int ro, int ri, uint16_t c) {
  if (a1 <= a0) return;
  while (a0 >= 360) { a0 -= 360; a1 -= 360; }
  ro -= 2; ri -= 2;                               // keep inside the 236-row canvas
  if (a1 > 360) { cv->fillArc(120, CY0, ro, ri, a0, 360, c); cv->fillArc(120, CY0, ro, ri, 0, a1 - 360, c); }
  else cv->fillArc(120, CY0, ro, ri, a0, a1, c);
}

void textC(const char *s, int cx, int cy, int size, uint16_t c) {
  int w = strlen(s) * 6 * size - size;
  cv->setTextSize(size);
  cv->setTextColor(c);
  cv->setCursor(cx - w / 2, cy - 4 * size);
  cv->print(s);
}

// 7-segment digit with rounded segments; d < 0 draws a dash
const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
void seg7(int x, int y, int w, int h, int t, int d, uint16_t c) {
  uint8_t m = d < 0 ? 0x40 : SEG[d % 10];
  int r = t / 2, hh = h / 2;
  if (m & 0x01) cv->fillRoundRect(x + r, y, w - 2 * r, t, r, c);              // a
  if (m & 0x02) cv->fillRoundRect(x + w - t, y + r, t, hh - r, r, c);         // b
  if (m & 0x04) cv->fillRoundRect(x + w - t, y + hh, t, hh - r, r, c);        // c
  if (m & 0x08) cv->fillRoundRect(x + r, y + h - t, w - 2 * r, t, r, c);      // d
  if (m & 0x10) cv->fillRoundRect(x, y + hh, t, hh - r, r, c);                // e
  if (m & 0x20) cv->fillRoundRect(x, y + r, t, hh - r, r, c);                 // f
  if (m & 0x40) cv->fillRoundRect(x + r, y + hh - t / 2, w - 2 * r, t, r, c); // g
}

// "AB:CD" centred on cx; a/b < 0 = dashes; blankLead hides a leading zero
void digits4(int a, int b, int cx, int y, int w, int h, int t, uint16_t c, bool colon, bool blankLead) {
  int gap = w / 4, colW = t * 2;
  int total = 4 * w + 4 * gap + colW;
  int x = cx - total / 2;
  if (!(blankLead && a >= 0 && a < 10)) seg7(x, y, w, h, t, a < 0 ? -1 : a / 10, c);
  seg7(x + w + gap, y, w, h, t, a < 0 ? -1 : a % 10, c);
  int xc = x + 2 * w + 2 * gap;
  if (colon) {
    cv->fillCircle(xc + colW / 2, y + h * 0.3f, t * 0.6f, c);
    cv->fillCircle(xc + colW / 2, y + h * 0.7f, t * 0.6f, c);
  }
  seg7(xc + colW + gap, y, w, h, t, b < 0 ? -1 : b / 10, c);
  seg7(xc + colW + 2 * gap + w, y, w, h, t, b < 0 ? -1 : b % 10, c);
}

// ---------- Hold feedback ----------
void drawHoldFeedback() {
  if (holdMs < 700) return;
  if (mode == M_FACES || mode == M_STOPWATCH) {   // 0.7 s = next mode, 3 s = auto on/off / reset
    float p = (holdMs - 700) / 2300.0f; if (p > 1) p = 1;
    ring(270, 270 + 360 * p, 119, 114, C_INK);
  } else {
    ring(0, 360, 119, 114, C_INK);
  }
}

// ---------- Timer ----------
unsigned long tmrLeft(unsigned long now) {
  if (tmrPaused) return tmrLeftAtPause;
  return (long)(tmrEnd - now) > 0 ? tmrEnd - now : 0;
}

void startBunnyTimer() {
  tmrTotal = TIMER_OPTS[tmrSel] * 60000UL;
  tmrEnd = millis() + tmrTotal;
  tmrPaused = false;
  setMode(M_TIMER_RUN);
  Serial.printf("Timer started: %d min\n", TIMER_OPTS[tmrSel]);
}

void drawTimerSet(unsigned long now) {
  textC("TIMER", 120, 50, 2, C_INK);
  int m = TIMER_OPTS[tmrSel];
  if (m >= 10) { seg7(72, 78, 42, 72, 10, m / 10, C_INK); seg7(126, 78, 42, 72, 10, m % 10, C_INK); }
  else seg7(99, 78, 42, 72, 10, m, C_INK);
  textC("MIN", 120, 168, 2, C_INK);
  for (int i = 0; i < 4; i++) cv->fillCircle(93 + i * 18, 192, i == tmrSel ? 5 : 3, i == tmrSel ? C_NOSE : C_GREY);
  textC("tap: change", 120, 210, 1, C_INK);
  // ring fills up while waiting to start
  float p = btnDown ? 0 : (now - lastSetPress) / (float)TIMER_START_DELAY; if (p > 1) p = 1;
  ring(0, 360, 119, 111, C_TRACK);
  ring(270, 270 + 360 * p, 119, 111, C_GREEN);
}

void drawTimerRun(unsigned long now) {
  unsigned long left = tmrLeft(now);
  float f = tmrTotal ? (float)left / tmrTotal : 0;
  bool blinkOff = tmrPaused && (now / 400) % 2;
  uint16_t c = f > 0.5f ? C_GREEN : f > 0.2f ? C_GOLD : C_ANGRY;
  if (left < 10000 && !tmrPaused && (now / 250) % 2) c = C_INK;      // last 10 s flash
  ring(0, 360, 119, 109, C_TRACK);
  if (!blinkOff) ring(270, 270 + 360 * f, 119, 109, c);
  textC("TIMER", 120, 50, 2, C_INK);
  unsigned long s = (left + 999) / 1000;
  if (!blinkOff) digits4(s / 60, s % 60, 120, 86, 34, 62, 9, C_INK, true, false);
  char b[16];
  if (tmrPaused) textC("PAUSED", 120, 172, 2, C_NOSE);
  else { snprintf(b, sizeof(b), "OF %d MIN", TIMER_OPTS[tmrSel]); textC(b, 120, 172, 2, C_INK); }
  textC(tmrPaused ? "tap: resume" : "tap: pause", 120, 200, 1, C_INK);
}

void drawTimerDone(unsigned long now) {
  ring(0, 360, 119, 109, ((now / 300) % 2) ? C_GOLD : C_HEART);
  cv->fillRoundRect(120 - 52, 184, 104, 28, 14, C_WHITE);
  textC("TIME UP!", 120, 198, 2, C_HEART);
}

// ---------- Stopwatch ----------
bool swRunning = false;
unsigned long swStart = 0, swAccum = 0;

unsigned long swElapsed(unsigned long now) { return swAccum + (swRunning ? now - swStart : 0); }

void drawStopwatch(unsigned long now) {
  unsigned long e = swElapsed(now);
  ring(0, 360, 119, 109, C_TRACK);
  ring(270, 270 + 360.0f * (e % 60000) / 60000.0f, 119, 109, swRunning ? C_NOSE : C_GREY);   // one lap per minute
  textC("STOPWATCH", 120, 50, 2, C_INK);
  unsigned long sec = e / 1000;
  if (sec < 3600) {
    digits4(sec / 60, sec % 60, 120, 78, 34, 62, 9, C_INK, true, false);
    int cs = (e / 10) % 100;                                  // hundredths
    seg7(98, 152, 18, 30, 5, cs / 10, C_NOSE);
    seg7(122, 152, 18, 30, 5, cs % 10, C_NOSE);
  } else {
    digits4(sec / 3600, (sec / 60) % 60, 120, 78, 34, 62, 9, C_INK, true, false);   // h:mm
    seg7(98, 152, 18, 30, 5, (sec % 60) / 10, C_NOSE);
    seg7(122, 152, 18, 30, 5, sec % 10, C_NOSE);
  }
  const char *hint = swRunning ? "tap: stop" : (e ? "tap: go  hold 3s: reset" : "tap: start");
  textC(hint, 120, 203, 1, C_INK);
}

// ---------- Colour menu ----------
void drawColour(unsigned long now) {
  float ts = (now - switchedAt) / 1000.0f;
  S = 1.0f - expf(-8 * ts) * cosf(14 * ts);
  CXf = 120; CYf = CY0 + sinf(now / 500.0f) * 2;
  if (S > 0.05f) e_smile(now / 1000.0f);
  textC("COLOUR", 120, 22, 2, C_INK);
  const char *n = THEME_NAMES[theme];
  int w = strlen(n) * 12 + 24;
  cv->fillRoundRect(120 - w / 2, 180, w, 28, 14, C_WHITE);
  cv->drawRoundRect(120 - w / 2, 180, w, 28, 14, C_INK);
  textC(n, 120, 194, 2, C_INK);
  for (int i = 0; i < NUM_THEMES; i++)
    cv->fillCircle(120 - (NUM_THEMES - 1) * 6 + i * 12, 220, i == theme ? 4 : 2, C_INK);
}

// ---------- Weather ----------
bool cfgSet(const char *s) { return s && s[0] && strncmp(s, "YOUR_", 5) != 0; }

String jsonVal(const String &p, const char *key) {
  String k = String("\"") + key + "\":";
  int i = p.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  if (p[i] == '"') { int j = p.indexOf('"', i + 1); return p.substring(i + 1, j); }
  int j = i;
  while (j < (int)p.length() && (isdigit(p[j]) || p[j] == '-' || p[j] == '.')) j++;
  return p.substring(i, j);
}

void weatherTask(void *) {
  unsigned long last = 0;
  bool first = true;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (first || millis() - last > WEATHER_EVERY_MS)) {
      HTTPClient http;
      String url = String("http://api.openweathermap.org/data/2.5/weather?q=") + OWM_CITY +
                   "&units=metric&appid=" + OWM_KEY;
      http.setTimeout(5000);
      int code = -1;
      if (http.begin(url)) {
        code = http.GET();
        if (code == 200) {
          String p = http.getString();
          String ic = jsonVal(p, "icon");
          String t  = jsonVal(p, "temp");
          if (ic.length() >= 3 && t.length()) {
            strncpy(wxCode, ic.c_str(), 3); wxCode[3] = 0;
            wxTemp = t.toFloat();
            wxValid = true;
            Serial.printf("Weather: %s, %.1f C (icon %s)\n", jsonVal(p, "main").c_str(), wxTemp, wxCode);
          }
        } else {
          Serial.printf("Weather error %d (401 = API key not active yet / wrong)\n", code);
        }
        http.end();
      }
      first = false;
      last = (code == 200) ? millis() : millis() - WEATHER_EVERY_MS + 60000;   // retry in 1 min on error
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// weather icons, in face coordinates (needs S = 1, CXf = CYf = 120)
void sunI(float x, float y, float s) {
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    thick(x + cosf(a) * s * 0.58f, y + sinf(a) * s * 0.58f, x + cosf(a) * s * 0.82f, y + sinf(a) * s * 0.82f, s * 0.1f, C_GOLD);
  }
  cv->fillCircle(X(x), Y(y), L(s * 0.42f), C_GOLD);
}
void moonI(float x, float y, float s) {
  cv->fillCircle(X(x), Y(y), L(s * 0.48f), C_GOLD);
  cv->fillCircle(X(x + s * 0.24f), Y(y - s * 0.18f), L(s * 0.42f), C_FACE);
}
void cloudI(float x, float y, float s, uint16_t c) {
  cv->fillCircle(X(x - s * 0.42f), Y(y + s * 0.08f), L(s * 0.34f), c);
  cv->fillCircle(X(x + s * 0.05f), Y(y - s * 0.12f), L(s * 0.46f), c);
  cv->fillCircle(X(x + s * 0.48f), Y(y + s * 0.1f), L(s * 0.32f), c);
  cv->fillRoundRect(X(x - s * 0.76f), Y(y + s * 0.05f), L(s * 1.52f), L(s * 0.37f), L(s * 0.18f), c);
}
void wxIcon(float x, float y, float s) {
  int code = atoi(wxCode);
  bool night = wxCode[2] == 'n';
  switch (code) {
    case 1:  night ? moonI(x, y, s) : sunI(x, y, s); break;
    case 2:  night ? moonI(x + s * 0.3f, y - s * 0.3f, s * 0.75f) : sunI(x + s * 0.3f, y - s * 0.3f, s * 0.75f);
             cloudI(x - s * 0.1f, y + s * 0.12f, s * 0.85f, C_CLOUD); break;
    case 9: case 10:
             cloudI(x, y - s * 0.15f, s, C_GREY);
             for (int i = -1; i <= 1; i++) thick(x + i * s * 0.35f, y + s * 0.38f, x + i * s * 0.35f - s * 0.12f, y + s * 0.72f, s * 0.1f, C_TEAR);
             break;
    case 11: cloudI(x, y - s * 0.15f, s, C_GREY);
             cv->fillTriangle(X(x + s * 0.12f), Y(y + s * 0.2f), X(x - s * 0.18f), Y(y + s * 0.52f), X(x + s * 0.04f), Y(y + s * 0.5f), C_GOLD);
             cv->fillTriangle(X(x + s * 0.04f), Y(y + s * 0.44f), X(x + s * 0.2f), Y(y + s * 0.46f), X(x - s * 0.08f), Y(y + s * 0.85f), C_GOLD);
             break;
    case 13: cloudI(x, y - s * 0.15f, s, C_CLOUD);
             for (int i = -1; i <= 1; i++) cv->fillCircle(X(x + i * s * 0.35f), Y(y + s * 0.55f + (i & 1) * s * 0.12f), L(s * 0.09f), C_BUBBLE);
             break;
    case 50: for (int i = 0; i < 3; i++) thick(x - s * 0.7f, y - s * 0.3f + i * s * 0.3f, x + s * 0.7f, y - s * 0.3f + i * s * 0.3f, s * 0.12f, C_GREY);
             break;
    default: cloudI(x, y, s, C_CLOUD); break;     // 03, 04 = clouds
  }
}

void tempText(int x, int cy, int size, uint16_t c) {      // left edge x, vertically centred on cy
  char b[8];
  snprintf(b, sizeof(b), "%d", (int)lroundf(wxTemp));
  cv->setTextSize(size); cv->setTextColor(c);
  cv->setCursor(x, cy - 4 * size); cv->print(b);
  int dx = x + strlen(b) * 6 * size + size;
  cv->drawCircle(dx + size, cy - 3 * size, size + 1, c);
  cv->drawCircle(dx + size, cy - 3 * size, size, c);
}

// ---------- Clock ----------
void hand(float deg, float len, float tail, float th, uint16_t c) {
  float a = deg * DEG_TO_RAD, sx = sinf(a), cy = -cosf(a);
  thick(-sx * tail, -cy * tail, sx * len, cy * len, th, c);
}

void drawClockStatus(bool ok) {
  if (ok) return;
  if (!cfgSet(WIFI_SSID)) textC("set WiFi in code", 120, 205, 1, C_INK);
  else if (WiFi.status() != WL_CONNECTED) textC("connecting WiFi...", 120, 205, 1, C_INK);
  else textC("getting time...", 120, 205, 1, C_INK);
}

void dateText(struct tm &t, char *b, size_t n, const char *fmt) {
  strftime(b, n, fmt, &t);
  for (char *p = b; *p; p++) *p = toupper(*p);
}

void drawDigital(struct tm &t, bool ok, unsigned long now) {
  ring(0, 360, 119, 113, C_TRACK);
  if (ok) ring(270, 270 + 6 * (t.tm_sec + 1), 119, 113, C_NOSE);       // seconds ring
  int h = -1, m = -1;
  if (ok) { h = CLOCK_24H ? t.tm_hour : (t.tm_hour + 11) % 12 + 1; m = t.tm_min; }
  digits4(h, m, 120, 88, 34, 62, 9, C_INK, (now / 500) % 2 == 0 || !ok, !CLOCK_24H);
  if (ok) {
    char b[20];
    dateText(t, b, sizeof(b), "%a %d %b");
    textC(b, 120, 172, 2, C_INK);
    if (!CLOCK_24H) textC(t.tm_hour < 12 ? "AM" : "PM", 120, 195, 2, C_NOSE);
  }
  if (wxValid) { wxIcon(-24, -62, 26); tempText(122, 58, 3, C_INK); }
  drawClockStatus(ok);
}

void drawAnalog(struct tm &t, bool ok, unsigned long now) {
  for (int i = 0; i < 60; i++) {
    float a = i * 6 * DEG_TO_RAD, sx = sinf(a), cy = -cosf(a);
    bool hr = i % 5 == 0;
    float r0 = hr ? 95 : 103;
    thick(sx * r0, cy * r0, sx * 109, cy * 109, hr ? 4 : 1.5f, hr ? C_INK : lerp565(C_FACE, C_INK, 0.4f));
  }
  textC("12", 120, 40, 2, C_INK);
  textC("3", 200, 120, 2, C_INK);
  textC("6", 120, 200, 2, C_INK);
  textC("9", 40, 120, 2, C_INK);
  if (ok) {
    char b[12];
    dateText(t, b, sizeof(b), "%d %b");
    textC(b, 120, 72, 2, C_INK);
  }
  if (wxValid) { wxIcon(-16, 50, 16); tempText(126, 170, 2, C_INK); }
  if (ok) {
    float sec = t.tm_sec, mn = t.tm_min + sec / 60.0f, hr = (t.tm_hour % 12) + mn / 60.0f;
    hand(hr * 30, 50, 8, 7, C_INK);
    hand(mn * 6, 80, 10, 5, C_INK);
    hand(sec * 6, 92, 16, 2, C_CARROT);
    float a = sec * 6 * DEG_TO_RAD;
    cv->fillCircle(X(sinf(a) * 92), Y(-cosf(a) * 92), 4, C_CARROT);
  }
  heart(0, 2, 16, C_NOSE);                                // bunny-nose centre cap
  cv->fillCircle(X(-3), Y(-2), 2, C_WHITE);
  drawClockStatus(ok);
}

void drawClock(unsigned long now) {
  S = 1; CXf = 120; CYf = CY0;
  struct tm t;
  bool ok = cfgSet(WIFI_SSID) && getLocalTime(&t, 0);
  if (analogClock) drawAnalog(t, ok, now); else drawDigital(t, ok, now);
}

// ---------- Setup / loop ----------
bool wifiStarted = false;

void startWiFi() {
  wifiStarted = true;
  if (!cfgSet(WIFI_SSID)) { Serial.println("WiFi not configured - clock/weather disabled"); return; }
  Serial.printf("Starting WiFi... free heap %u\n", ESP.getFreeHeap());
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);      // helps the C3 SuperMini's small antenna connect
  WiFi.setAutoReconnect(true);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  if (cfgSet(OWM_KEY) && cfgSet(OWM_CITY)) xTaskCreate(weatherTask, "weather", 8192, NULL, 1, NULL);
}

const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "CRASH (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power supply dipped)";
    case ESP_RST_USB:      return "USB reset";
    default:               return "other";
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(20);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(FACE_BTN, INPUT_PULLUP);
  pinMode(TFT_BL, OUTPUT);
  setBacklight(0);                         // keep the screen dark until there is a picture
  delay(1500);                             // give the Serial Monitor time to connect

  Serial.println("\n=== Bunny Buddy ===");
  Serial.printf("Reset reason: %s\n", resetReasonText());
  Serial.printf("Free heap: %u bytes, largest block: %u bytes (screen buffer needs 113280)\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (!cv->begin(40000000)) {
    while (true) {
      Serial.printf("Display init FAILED - not enough RAM for the screen buffer (largest block %u)\n", ESP.getMaxAllocHeap());
      delay(2000);
    }
  }
  prefs.begin("bunny", false);
  applyTheme(prefs.getUChar("theme", 0));
  panel->fillRect(0, 0, 240, CANVAS_Y, C_FACE);                              // rows the canvas never covers
  panel->fillRect(0, CANVAS_Y + CANVAS_H, 240, 240 - CANVAS_Y - CANVAS_H, C_FACE);
  Serial.println("Display OK");
  randomSeed(esp_random());

  autoMode    = prefs.getBool("auto", true);
  analogClock = prefs.getBool("analog", true);
  tmrSel      = prefs.getUChar("tsel", 1) % 4;
  setExpr(prefs.getUChar("expr", 0), false);

  Serial.println("Expressions:");
  for (int i = 0; i < NUM_EXPR; i++) Serial.printf("  %d = %s\n", i, EXPR[i].name);
  Serial.println("WiFi starts in 3 seconds...");
}

void loop() {
  unsigned long now = millis();
  if (!wifiStarted && now > 4500) startWiFi();        // start WiFi only after the face is showing
  Ev ev = readButton(now);
  handleSerial();

  switch (mode) {
    case M_FACES:
      if (ev == EV_SHORT) setExpr(cur + 1, true);
      else if (ev == EV_LONG) setMode(M_TIMER_SET);
      else if (ev == EV_VLONG) {
        autoMode = !autoMode; ringUntil = now + 1000;
        prefs.putBool("auto", autoMode);
        Serial.printf("Auto mode %s\n", autoMode ? "ON" : "OFF");
      }
      break;
    case M_TIMER_SET:
      if (ev == EV_SHORT) { tmrSel = (tmrSel + 1) % 4; lastSetPress = now; prefs.putUChar("tsel", tmrSel); }
      else if (ev == EV_LONG) setMode(M_STOPWATCH);
      else if (!btnDown && now - lastSetPress > TIMER_START_DELAY) startBunnyTimer();
      break;
    case M_TIMER_RUN:
      if (ev == EV_SHORT) {
        if (tmrPaused) { tmrEnd = now + tmrLeftAtPause; tmrPaused = false; }
        else { tmrLeftAtPause = tmrLeft(now); tmrPaused = true; }
      }
      else if (ev == EV_LONG) setMode(M_FACES);
      else if (tmrLeft(now) == 0) setMode(M_TIMER_DONE);
      break;
    case M_TIMER_DONE:
      if (ev == EV_SHORT || ev == EV_LONG || now - modeAt > 60000) setMode(M_FACES);
      break;
    case M_STOPWATCH:
      if (ev == EV_SHORT) {
        if (swRunning) { swAccum += now - swStart; swRunning = false; }
        else { swStart = now; swRunning = true; }
      }
      else if (ev == EV_VLONG && !swRunning) swAccum = 0;          // hold 3 s while stopped = reset
      else if (ev == EV_LONG) setMode(M_CLOCK);
      break;
    case M_CLOCK:
      if (ev == EV_SHORT) { analogClock = !analogClock; prefs.putBool("analog", analogClock); }
      else if (ev == EV_LONG) setMode(M_COLOUR);
      break;
    case M_COLOUR:
      if (ev == EV_SHORT) {
        applyTheme(theme + 1); prefs.putUChar("theme", theme); switchedAt = now;
        panel->fillRect(0, 0, 240, CANVAS_Y, C_FACE);
        panel->fillRect(0, CANVAS_Y + CANVAS_H, 240, 240 - CANVAS_Y - CANVAS_H, C_FACE);
      }
      else if (ev == EV_LONG) setMode(M_FACES);
      break;
  }

  bool faceMode = (mode == M_FACES || mode == M_TIMER_DONE);
  if (mode == M_FACES && autoMode && now - switchedAt > AUTO_INTERVAL_MS) {
    int n = random(NUM_EXPR - 1);
    setExpr(n >= cur ? n + 1 : n, false);
  }

  updateLife(now);
  cv->fillScreen(C_FACE);

  if (faceMode) {
    // features pop in with a springy bounce on change, then gentle breathing
    float ts = (now - switchedAt) / 1000.0f;
    S   = 1.0f - expf(-8 * ts) * cosf(14 * ts);
    CXf = 120;
    CYf = CY0 + sinf(now / 500.0f) * 2;
    if (S > 0.05f) EXPR[cur].draw(now / 1000.0f);
    if (mode == M_TIMER_DONE) drawTimerDone(now);
  } else if (mode == M_TIMER_SET) drawTimerSet(now);
  else if (mode == M_TIMER_RUN)   drawTimerRun(now);
  else if (mode == M_STOPWATCH)   drawStopwatch(now);
  else if (mode == M_COLOUR)      drawColour(now);
  else                            drawClock(now);

  if (now < ringUntil) ring(0, 360, 119, 112, autoMode ? C_GREEN : C_GREY);
  drawHoldFeedback();
  cv->flush();
}
