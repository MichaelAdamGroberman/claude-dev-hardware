#include <M5StickCPlus.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "mic.h"
#include "net_wifi.h"
#include "net_wg.h"
#include "net_tcp.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);
// Offscreen sprite for the landscape-clock pet area. Eliminates the
// fillRect/redraw flash that direct-to-LCD rendering produced; the
// pet is composed atomically in this sprite then blitted to the LCD.
TFT_eSprite petSpr = TFT_eSprite(&M5.Lcd);
bool petSprReady = false;

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;
const int LED_PIN = 10;          // red LED, active-low

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

// ── Type scale (one scale, applied uniformly across every draw* function) ──
// Adafruit-GFX text sizes map to roles. The rule: a given semantic ROLE uses
// the SAME size + color on every screen, so the eye learns the hierarchy once.
//   FS_HERO   = 3  — big numbers / huge tool name (the one thing per screen
//                    you read first: token hero, passkey digits, tool name).
//   FS_HEADER = 2  — screen/panel header labels (MENU, USAGE, BLUETOOTH…),
//                    and the per-screen lead/status WORD (e.g. "LINKED").
//   FS_BODY   = 2  — the legibility floor: list rows, values, status text.
//                    Never drop a value/row below this.
//   FS_SUB    = 1  — true captions ONLY: page counters ("3/7"), footer
//                    key-hints, the dim label beside a value. For wide data
//                    rows (kv pairs that don't fit at size 2) prefer the
//                    built-in Font 2 (8×16) over size 1 — see lg() in drawInfo.
const uint8_t FS_HERO   = 3;
const uint8_t FS_HEADER = 2;
const uint8_t FS_BODY   = 2;
const uint8_t FS_SUB    = 1;

// ── Design-system palette (screens.jsx) ──────────────────────────────────
// RGB565 of the canvas hex tokens. Names mirror the design so a screen body
// reads close to the JSX. These are FIXED accent colors (independent of the
// per-character Palette tint) so every screen reads consistently across
// buddy skins, exactly as the canvas intends.
const uint16_t DS_WHITE   = 0xFFFF;   // #ffffff white values
const uint16_t DS_BLACK   = 0x0000;   // #000000 bg / text-on-accent
const uint16_t DS_DIM     = 0x7C32;   // #7a8492 dim labels
const uint16_t DS_GREEN   = 0x2EAB;   // #29d65b status ok / busy / encrypted
const uint16_t DS_ORANGE  = 0xFC63;   // #ff8c1a selection highlight / accent
const uint16_t DS_CYAN    = 0x05FF;   // #00bdff idle status / passkey border
const uint16_t DS_RED     = 0xF943;   // #ff2a1a alarm
const uint16_t DS_AMBER   = 0xFEA1;   // #ffd60a charging / adapter / evolution
const uint16_t DS_PINK    = 0xF972;   // #ff2d92 stage-up ascended / DJ
const uint16_t DS_REDSOFT = 0xFB6B;   // #ff6e5b denied / destructive value
const uint16_t DS_GRNSOFT = 0x5FEF;   // #5cff7a cancel action fg
const uint16_t DS_LINE    = 0x18E3;   // #1a1c1e hairline dividers / borders
const uint16_t DS_PANEL   = 0x0882;   // #0e1014 inset chip background
const uint16_t DS_HEADER  = 0x18C4;   // #181a22 menu header strip bg
const uint16_t DS_HDRBORD = 0x2967;   // #2a2f3a menu header bottom border
const uint16_t DS_OFFBAR  = 0x2146;   // #262a32 empty mini-bar outline
const uint16_t DS_OFFDOT  = 0x39E9;   // #3a3f48 empty mini-dot outline
const uint16_t DS_PASSHDR = 0x19CD;   // #1a3a6b passkey header bg
const uint16_t DS_DARK    = 0x0861;   // #0a0c0e darkest inset / footer bg
const uint16_t DS_CFMBG   = 0x1821;   // #1a0408 destructive-confirm bg
const uint16_t DS_CFMHDR  = 0xC9C7;   // #c83a3a destructive-confirm header
const uint16_t DS_DENYBG  = 0x3841;   // #3a0a08 deny/reset action bg
const uint16_t DS_CXLBG   = 0x0943;   // #0a2a18 cancel action bg
const uint16_t DS_ADPHDR  = 0x3941;   // #3a2a08 adapter header bg
const uint16_t DS_SUTOP   = 0x1846;   // #1a0832 stage-up gradient top
const uint16_t DS_SUBOT   = 0x0801;   // #08020a stage-up gradient bottom

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

// gr0m ride-mode controls (#tilt-skate/#tilt-surf/#tilt-hover) — implemented
// in gr0m.cpp. Cycle off→skate→surf→hover→off via the settings "ride" row.
extern void    gr0mCycleRide();
extern uint8_t gr0mRideMode();

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
// Pet pages: 0 = stats, 1 = how-to, 2 = evolution showcase. The showcase page
// cycles the DISPLAYED evolution stage 0..5 so the character shows off every
// upgrade it has unlocked (and previews what's still coming).
const uint8_t PET_PAGES = 3;
const uint8_t PET_PG_SHOWCASE = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;

// Mirrors read by gr0m.cpp each frame — stats.h state is file-static and can't
// be shared across translation units, so the renderer reads these instead of
// stats() (which would be a stale, never-updated copy in gr0m.cpp).
uint8_t  g_evoStage   = 0;   // = evoStage()
uint32_t g_dispTokens = 0;   // = stats().tokens (period figure for the chest-LCD)
bool     gifAvailable = false;

// Evolution showcase: a non-destructive demo (Pet page PET_PG_SHOWCASE) that
// walks the DISPLAYED stage 0..5 so the character shows off everything it has
// unlocked and previews what's coming. It only OVERRIDES the g_evoStage mirror
// that gr0m.cpp renders from; it never touches the persisted lifetimeTokens /
// real evoStage(). The mirror is re-derived from evoStage() every loop (see the
// "refresh the mirrors" line in loop()), so leaving the page restores truth for
// free. SHOWCASE_HOLD_MS is the dwell per stage (~1.5s) before advancing.
const uint32_t SHOWCASE_HOLD_MS = 1500;
static uint32_t showcaseStartMs = 0;   // millis() when the showcase page was entered
// Human-readable stage names, matching the gr0m.cpp render comments:
//   0 Core, 1 Frame, 2 Powered, 3 Persona, 4 HUD, 5 Ascended.
static const char* const EVO_STAGE_NAMES[6] = {
  "Core", "Frame", "Powered", "Persona", "HUD", "Ascended"
};
// Stage the showcase is currently displaying, derived purely from elapsed time
// since the page was entered. Loops 0..5 → 0..5 forever.
static inline uint8_t showcaseStage() {
  uint32_t elapsed = millis() - showcaseStartMs;
  return (uint8_t)((elapsed / SHOWCASE_HOLD_MS) % 6);
}
// True while the Pet showcase page owns the g_evoStage mirror. The caller in
// loop() additionally requires no overlay menu to be open (those bools are
// declared further down) before applying the override.
static inline bool showcasePageActive() {
  return displayMode == DISP_PET && petPage == PET_PG_SHOWCASE && !menuOpen;
}
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative (SPEC #tilt-nap: gravity-down
// < -0.7g). Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

// Picked back up — SPEC #tilt-nap exit: |gravity-down| < 0.5g. The 0.5..0.7g
// band is a deliberate hysteresis gap so the nap doesn't bounce at the edge.
static bool isFaceUp() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return fabsf(az) < 0.5f;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

// Emit a device→daemon event line over Serial + BLE (mirrors _xAck in
// xfer.h). The daemon reads these device→host lines and acts on them; the
// Usage menu uses it for the period / token_reset / level_reset controls.
//   sendEvt("period", "\"value\":\"week\"")  → {"evt":"period","value":"week"}
//   sendEvt("token_reset", nullptr)         → {"evt":"token_reset"}
static void sendEvt(const char* evt, const char* extraJson) {
  char b[80];
  int len = (extraJson && *extraJson)
    ? snprintf(b, sizeof(b), "{\"evt\":\"%s\",%s}\n", evt, extraJson)
    : snprintf(b, sizeof(b), "{\"evt\":\"%s\"}\n", evt);
  Serial.write(b, len);
  bleWrite((const uint8_t*)b, len);
}
const uint8_t INFO_PAGES = 7;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CONNECTIONS = 4;
const uint8_t INFO_PG_USAGE = 5;
const uint8_t INFO_PG_CREDITS = 6;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
// Radio + adapter controls moved to the Connection submenu (drawConn). DJ is no
// longer a manual setting — it's an automatic end-game flourish (see djActive in
// loop(), unlocked at Stage 5), so there's no "dj" row here.
// Labels kept short so each row fits at BODY size (size 2) alongside the
// right-aligned value column in the shared list look (drawListMenu). Indices
// must stay aligned with applySetting()/settingsMeta(): 0 bright, 1 sound,
// 2 led, 3 mic, 4 transcript(hud), 5 clock rot, 6 worldUp (#tilt-worldup),
// 7 ride mode (#tilt-skate/surf/hover), 8 ascii pet, 9 reset, 10 back.
const char* settingsItems[] = { "bright", "sound", "led", "mic", "hud", "clock", "world", "ride", "pet", "reset", "back" };
const uint8_t SETTINGS_N = 11;
extern bool adapterMode;   // defined near loop(); the Connection menu toggles it

// Connection submenu — radio (WiFi/BT/Off) + Adapter, opened from the
// CONNECTIONS info page (infoPage==4) via a BtnA long-press.
bool    connOpen = false;
uint8_t connSel  = 0;
const char* connItems[] = { "WiFi", "BT", "Off", "Adapter", "Back" };
const uint8_t CONN_N = 5;

// Usage submenu — reporting span + period/level resets, opened from the
// USAGE info page (INFO_PG_USAGE) via a BtnA long-press.
bool    usageOpen = false;
uint8_t usageSel  = 0;
const char* usageItems[] = { "span", "reset count", "reset level", "Back" };
const uint8_t USAGE_N = 4;
// Local mirror of the daemon reporting window. Cycles day→week→month→all.
static const char* const USAGE_SPANS[]  = { "1 day", "7 days", "30 days", "Full" };
static const char* const USAGE_SPANVAL[] = { "day", "week", "month", "all" };
static uint8_t usageSpanIdx = 0;
// Tap-twice confirm for the two usage resets (mirrors applyReset's pattern).
static uint32_t usageConfirmUntil = 0;
static uint8_t  usageConfirmIdx   = 0xFF;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2: s.led = !s.led; break;
    case 3: s.mic = !s.mic; micSetEnabled(s.mic); break;
    case 4: s.hud = !s.hud; break;   // transcript
    case 5: s.clockRot = (s.clockRot + 1) % 3; break;
    case 6: s.worldUp = !s.worldUp;  // #tilt-worldup
            // Clear any worldUp rotation immediately when turning it off so
            // the home view snaps back upright without waiting for a re-tilt.
            if (!s.worldUp) M5.Lcd.setRotation(0);
            break;
    case 7: gr0mCycleRide(); return; // #tilt-ride: off→skate→surf→hover→off
    case 8: nextPet(); return;       // ascii pet
    case 9: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 10: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// ── Design-system shared draw helpers (screens.jsx atoms) ─────────────────
// Small pixel-aligned primitives reused across the redesigned screens, ported
// from the JSX DotIcon / BatteryIcon / MiniDots / MiniBars atoms. All draw
// into the global `spr` sprite at the given top-left/anchor.

// Status dot — a solid filled circle, optional soft halo ring (the JSX
// boxShadow). Used in headers and the BT/adapter status lines.
static void drawStatusDot(int cx, int cy, uint16_t col, int r = 3, bool halo = true) {
  if (halo) spr.drawCircle(cx, cy, r + 1, col);
  spr.fillCircle(cx, cy, r, col);
}

// Battery glyph: outlined shell + terminal nub + colored fill bar. Mirrors the
// JSX BatteryIcon — fill width tracks pct, color steps red/amber/green. Drawn
// from top-left (x,y); total footprint is (w+3) × h. Returns nothing.
static void drawBatteryGlyph(int x, int y, uint8_t pct, int w = 18, int h = 9) {
  if (pct > 100) pct = 100;
  uint16_t col = (pct < 15) ? DS_RED : (pct < 35) ? DS_ORANGE : DS_GREEN;
  spr.drawRect(x, y, w, h, DS_DIM);                 // shell outline
  spr.fillRect(x + w, y + 2, 2, h - 4, DS_DIM);     // terminal nub
  int fillW = ((w - 4) * pct) / 100;
  if (fillW < 1) fillW = 1;
  spr.fillRect(x + 2, y + 2, fillW, h - 4, col);    // fill bar
}

// Header strip — the home-screen top bar: "gr0m" (white, size 1 bold-ish) on
// the left; a status dot + state label (colored) + battery glyph on the right;
// a hairline divider underneath. Drawn into the top `H_STRIP` px of the screen.
// `pct` 0..100; `stateCol`/`stateLbl` carry the IDLE(cyan)/BUSY(green) state.
const int H_STRIP = 22;
static void drawHeaderStrip(const char* stateLbl, uint16_t stateCol, uint8_t pct) {
  spr.fillRect(0, 0, W, H_STRIP, DS_BLACK);
  spr.setTextSize(1);
  spr.setTextColor(DS_WHITE, DS_BLACK);
  spr.setCursor(6, 4);
  spr.print("gr0m");
  // Right cluster, laid out right-to-left: battery (18+3 wide) | state | dot.
  int bx = W - 6 - 21;
  drawBatteryGlyph(bx, 4, pct);
  int slen = strlen(stateLbl);
  int sx = bx - 6 - slen * 6;
  spr.setTextColor(stateCol, DS_BLACK);
  spr.setCursor(sx, 4);
  spr.print(stateLbl);
  drawStatusDot(sx - 8, 7, stateCol, 3, true);
  spr.drawFastHLine(0, H_STRIP - 1, W, DS_LINE);
}

// MiniDots — a row of `max` small circles, first `value` filled (color), the
// rest hollow (dim outline). Ported from the JSX MiniDots atom. Returns the x
// just past the row so callers can chain.
static int drawMiniDots(int x, int y, uint8_t value, uint8_t mx,
                        uint16_t col, int r = 3, int gap = 4) {
  int step = r * 2 + gap;
  for (uint8_t i = 0; i < mx; i++) {
    int cx = x + r + i * step;
    if (i < value) spr.fillCircle(cx, y, r, col);
    else           spr.drawCircle(cx, y, r, DS_OFFDOT);
  }
  return x + mx * step;
}

// MiniBars — a row of `max` small rectangles, first `value` filled (color), the
// rest hollow (dim outline). Ported from the JSX MiniBars atom.
static int drawMiniBars(int x, int y, uint8_t value, uint8_t mx,
                        uint16_t col, int w = 7, int h = 9, int gap = 2) {
  int step = w + gap;
  for (uint8_t i = 0; i < mx; i++) {
    int bx = x + i * step;
    if (i < value) spr.fillRect(bx, y, w, h, col);
    else           spr.drawRect(bx, y, w, h, DS_OFFBAR);
  }
  return x + mx * step;
}

// Short token formatter (e.g. 31200 → "31.2K", 184000 → "184K", 3_700_000 →
// "3.7M"). Writes into caller's buffer and returns it. Shared by the home hero
// and the sessions strip. `unitOut` (optional) receives the trailing unit char
// so callers can render it dim/smaller per the design (e.g. "31.2" + dim "K").
static const char* fmtTokens(char* buf, size_t bsz, uint32_t v, char* unitOut = nullptr) {
  char unit = 0;
  // ≥100K and ≥10M: no decimal (3 digits is plenty). Below that: 1 decimal,
  // matching the canvas figures "31.2K" / "184K" / "3.7M".
  if      (v >= 1000000000UL) { snprintf(buf, bsz, "%lu.%lu", (unsigned long)(v/1000000000UL), (unsigned long)((v/100000000UL)%10)); unit='B'; }
  else if (v >= 10000000UL)   { snprintf(buf, bsz, "%lu", (unsigned long)(v/1000000UL)); unit='M'; }
  else if (v >= 1000000UL)    { snprintf(buf, bsz, "%lu.%lu", (unsigned long)(v/1000000UL), (unsigned long)((v/100000UL)%10)); unit='M'; }
  else if (v >= 100000UL)     { snprintf(buf, bsz, "%lu", (unsigned long)(v/1000UL)); unit='K'; }
  else if (v >= 1000UL)       { snprintf(buf, bsz, "%lu.%lu", (unsigned long)(v/1000UL), (unsigned long)((v/100UL)%10)); unit='K'; }
  else                        { snprintf(buf, bsz, "%lu", (unsigned long)v); }
  if (unitOut) *unitOut = unit;
  return buf;
}

// Battery percentage from the AXP192 (same derivation drawPetStats / the DEVICE
// info page use): (Vbat − 3.2V) scaled to 0..100. Clamped.
static uint8_t batteryPct() {
  int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
  int pct = (vBat_mV - 3200) / 10;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

// ── Shared full-screen list menu (screens.jsx ScreenMenu/UsageMenu/ConnMenu)
// All three list submenus share one look: a dark header strip with the title
// (size 2) + "sel+1/total" counter, full-width rows where the SELECTED row is a
// solid orange band with a ">" marker and black text (per-row danger rows go
// red when selected), and a footer hint strip. Rendered full-screen — the
// canvas screens own the whole 135×240 viewport — with size-2 body labels.
//
// A row's right-hand meta string is supplied by `metaFn` (label + color +
// danger flag), so the same renderer drives the demo toggle, the active-radio
// check, the span label, and the destructive-row coloring.
typedef void (*ListMetaFn)(int idx, char* out, size_t osz, uint16_t* col, bool* danger);

static void drawListMenu(const char* title, const char* const* items, int n,
                         int sel, const char* footL, const char* footR,
                         ListMetaFn metaFn) {
  spr.fillSprite(DS_BLACK);
  const int HEADER_H = 20, FOOTER_H = 14;
  // Header strip — title at HEADER size, the n/total counter at SUB.
  spr.fillRect(0, 0, W, HEADER_H, DS_HEADER);
  spr.drawFastHLine(0, HEADER_H, W, DS_HDRBORD);
  spr.setTextSize(FS_HEADER);
  spr.setTextColor(DS_WHITE, DS_HEADER);
  spr.setCursor(6, 3);
  spr.print(title);
  char cb[12]; snprintf(cb, sizeof(cb), "%d/%d", sel + 1, n);
  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_DIM, DS_HEADER);
  spr.setCursor(W - 6 - (int)strlen(cb) * 6, 7);
  spr.print(cb);

  // Rows — evenly fill the band between header and footer.
  const int rowsTop = HEADER_H + 3;
  const int avail   = H - rowsTop - FOOTER_H - 2;
  const int ROW_H   = avail / (n > 0 ? n : 1);
  for (int i = 0; i < n; i++) {
    int ry = rowsTop + i * ROW_H;
    char meta[16] = ""; uint16_t mcol = DS_DIM; bool danger = false;
    if (metaFn) metaFn(i, meta, sizeof(meta), &mcol, &danger);
    bool sel_ = (i == sel);
    uint16_t rowBg    = sel_ ? (danger ? DS_CFMHDR : DS_ORANGE) : DS_BLACK;
    uint16_t labelCol = sel_ ? DS_BLACK : (danger ? DS_REDSOFT : DS_WHITE);
    if (sel_) spr.fillRect(0, ry, W, ROW_H, rowBg);
    spr.setTextSize(FS_BODY);   // rows are BODY (the legibility floor)
    if (sel_) { spr.setTextColor(DS_BLACK, rowBg); spr.setCursor(4, ry + (ROW_H - 16) / 2); spr.print(">"); }
    spr.setTextColor(labelCol, sel_ ? rowBg : DS_BLACK);
    spr.setCursor(18, ry + (ROW_H - 16) / 2);
    spr.print(items[i]);
    if (meta[0]) {
      spr.setTextSize(FS_SUB);   // value column = SUB caption
      spr.setTextColor(sel_ ? DS_BLACK : mcol, sel_ ? rowBg : DS_BLACK);
      spr.setCursor(W - 6 - (int)strlen(meta) * 6, ry + (ROW_H - 8) / 2);
      spr.print(meta);
    }
  }

  // Footer hint strip — key-hints are SUB captions.
  int fy = H - FOOTER_H;
  spr.fillRect(0, fy, W, FOOTER_H, DS_DARK);
  spr.drawFastHLine(0, fy, W, DS_LINE);
  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_DIM, DS_DARK);
  spr.setCursor(6, fy + 4);
  spr.print(footL);
  spr.setCursor(W - 6 - (int)strlen(footR) * 6, fy + 4);
  spr.print(footR);
}

// Settings submenu meta — value column per row, using the shared list look
// (DS palette, size-1 caption meta). On/off rows go green/dim; brightness,
// clock-rot and ascii-pet show their cycler value. Reset (idx 7) carries the
// destructive "!" marker; back (idx 8) has no meta.
static void settingsMeta(int i, char* out, size_t osz, uint16_t* col, bool* danger) {
  Settings& s = settings();
  if (i == 0) {
    snprintf(out, osz, "%u/4", brightLevel); *col = DS_ORANGE;
  } else if (i >= 1 && i <= 4) {
    bool on = (i == 1) ? s.sound : (i == 2) ? s.led : (i == 3) ? s.mic : s.hud;
    snprintf(out, osz, "%s", on ? "on" : "off");
    *col = on ? DS_GREEN : DS_DIM;
  } else if (i == 5) {
    static const char* const RN[] = { "auto", "port", "land" };
    snprintf(out, osz, "%s", RN[s.clockRot]); *col = DS_ORANGE;
  } else if (i == 6) {
    snprintf(out, osz, "%s", s.worldUp ? "on" : "off");
    *col = s.worldUp ? DS_GREEN : DS_DIM;
  } else if (i == 7) {
    static const char* const RD[] = { "off", "skate", "surf", "hover" };
    snprintf(out, osz, "%s", RD[gr0mRideMode() & 3]);
    *col = (gr0mRideMode() != 0) ? DS_GREEN : DS_DIM;
  } else if (i == 8) {
    uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
    uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
    snprintf(out, osz, "%u/%u", pos, total); *col = DS_ORANGE;
  } else if (i == 9) {
    snprintf(out, osz, "!"); *col = DS_REDSOFT; *danger = true;
  }
}
// Settings — now the shared full-screen list look (ScreenMenu family) so it
// matches MENU / RADIO / USAGE / RESET. BtnA scrolls, BtnB changes the row.
static void drawSettings() {
  drawListMenu("SETTINGS", settingsItems, SETTINGS_N, settingsSel,
               "A scroll", "B change", settingsMeta);
}

// Reset submenu meta — "delete char" / "factory reset" are both destructive.
static void resetMeta(int i, char* out, size_t osz, uint16_t* col, bool* danger) {
  if (i < 2) { snprintf(out, osz, "!"); *col = DS_REDSOFT; *danger = true; }
}

// ScreenConfirmDestructive — the full-screen tap-twice confirm overlay. Shown
// (over the list) once a destructive row is armed. Red header + "tap n/2" +
// the action sentence + an A reset / B cancel action footer. The firmware's
// arm/execute model is two taps total (arm → confirm), so the armed state is
// "tap 1/2" awaiting the final tap.
static void drawResetConfirm(const char* action, const char* l1, const char* l2,
                             const char* l3) {
  spr.fillSprite(DS_CFMBG);
  // Red header bar
  spr.fillRect(0, 0, W, 22, DS_CFMHDR);
  spr.setTextSize(1);
  spr.setTextColor(DS_BLACK, DS_CFMHDR);
  spr.setCursor(6, 3);  spr.print("! DESTRUCTIVE");
  spr.setTextSize(2);
  spr.setTextColor(DS_BLACK, DS_CFMHDR);
  spr.setCursor(W - 6 - 4 * 12, 3);  spr.print("1/2");

  // Body — action label (dim red) + big white sentence, centered-ish.
  int y = 56;
  spr.setTextSize(1);
  spr.setTextColor(DS_REDSOFT, DS_CFMBG);
  spr.setCursor((W - (int)strlen(action) * 6) / 2, y); spr.print(action);
  y += 18;
  spr.setTextSize(2);
  spr.setTextColor(DS_WHITE, DS_CFMBG);
  const char* lines[3] = { l1, l2, l3 };
  for (int i = 0; i < 3; i++) {
    if (!lines[i] || !lines[i][0]) continue;
    spr.setCursor((W - (int)strlen(lines[i]) * 12) / 2, y); spr.print(lines[i]);
    y += 20;
  }
  y += 8;
  spr.setTextSize(1);
  spr.setTextColor(DS_DIM, DS_CFMBG);
  const char* hint = "tap B to confirm";
  spr.setCursor((W - (int)strlen(hint) * 6) / 2, y); spr.print(hint);

  // Action footer. Button semantics (see loop()): on the reset menu BtnB
  // confirms/executes the armed row; BtnA scrolls (which disarms = cancel).
  // So the footer reads A cancel (green) | B reset (red), matching reality.
  const int FH = 34, FY = H - FH;
  spr.fillRect(0,     FY, W / 2,     FH, DS_CXLBG);
  spr.fillRect(W / 2, FY, W - W / 2, FH, DS_DENYBG);
  spr.drawFastVLine(W / 2, FY, FH, DS_BLACK);
  spr.setTextColor(DS_GRNSOFT, DS_CXLBG);
  spr.setCursor(8, FY + 12);          spr.print("A cancel");
  spr.setTextColor(DS_REDSOFT, DS_DENYBG);
  spr.setCursor(W / 2 + 8, FY + 12);  spr.print("B reset");
}

static void drawReset() {
  // Armed → full-screen destructive confirm (ScreenConfirmDestructive).
  bool armed = (int32_t)(millis() - resetConfirmUntil) < 0 && resetConfirmIdx < 2;
  if (armed) {
    if (resetConfirmIdx == 0) drawResetConfirm("delete char", "wipe the", "installed", "character?");
    else                      drawResetConfirm("factory reset", "erase ALL", "data and", "re-pair?");
    return;
  }
  // Otherwise the destructive list, shared look, red danger meta.
  drawListMenu("RESET", resetItems, RESET_N, resetSel, "A scroll", "B confirm", resetMeta);
}

// Connection submenu — ScreenConnMenu ("RADIO"). The meta column reads LIVE
// radio state each frame (drawListMenu re-runs every loop while connOpen), so
// the WiFi/BT rows reflect what the radio is actually doing right now — not
// just the persisted setting. WiFi shows STA/AP state; BT shows link or the
// "suspend" the WiFi/BLE mutex parks it in; Off is green only when both radios
// are genuinely down.
static void connMeta(int i, char* out, size_t osz, uint16_t* col, bool* danger) {
  Settings& s = settings();
  if (i == 0) {                    // WiFi — live STA/AP state
    if (!s.wifi) return;
    NetWifiState ws = netWifiState();
    switch (ws) {
      case NW_ONLINE:     snprintf(out, osz, "online"); *col = DS_GREEN;  break;
      case NW_PORTAL:     snprintf(out, osz, "AP %d", netWifiApClients()); *col = DS_AMBER; break;
      case NW_CONNECTING:
      case NW_STARTING:   snprintf(out, osz, "...");    *col = DS_AMBER;  break;
      case NW_FAILED:     snprintf(out, osz, "failed"); *col = DS_RED;    break;
      default:            snprintf(out, osz, "on");     *col = DS_DIM;    break;
    }
  } else if (i == 1) {             // BT — live link / suspended-by-WiFi
    if (!s.bt) return;
    if (bleSuspended())      { snprintf(out, osz, "suspend"); *col = DS_DIM; }
    else if (bleConnected()) { snprintf(out, osz, "linked");  *col = DS_GREEN; }
    else                     { snprintf(out, osz, "on");      *col = DS_AMBER; }
  } else if (i == 2) {             // Off — green when both radios are really off
    if (!s.wifi && !s.bt) { snprintf(out, osz, "ON"); *col = DS_GREEN; }
  }
}
static void drawConn() {
  drawListMenu("RADIO", connItems, CONN_N, connSel, "WiFi+BT", "reboots", connMeta);
}

static void applyConn(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:  // WiFi — mutually exclusive with BT; reboot into WiFi mode.
      s.wifi = true; s.bt = false;
      settingsSave();
      Serial.println("[conn] WiFi ON (bt off) — rebooting");
      delay(150);
      ESP.restart();
      return;
    case 1:  // BT — mutually exclusive with WiFi; reboot into BT mode.
      s.bt = true; s.wifi = false;
      settingsSave();
      Serial.println("[conn] BT ON (wifi off) — rebooting");
      delay(150);
      ESP.restart();
      return;
    case 2:  // Off — both radios off; reboot so the radio comes up clean.
      s.wifi = false; s.bt = false;
      settingsSave();
      Serial.println("[conn] radios OFF — rebooting");
      delay(150);
      ESP.restart();
      return;
    case 3:  // Adapter (GPIO/logic-probe) mode — runtime; BtnB or reset exits.
      adapterMode = true;
      if (!bleConnected()) bleAdvertisingStop();
      connOpen = false;
      characterInvalidate();
      return;
    case 4:  // Back
      connOpen = false;
      characterInvalidate();
      return;
  }
}

// Usage submenu — ScreenUsageMenu ("USAGE"). The span row carries the current
// reporting window as meta (cycler); "reset level" (index 2) is destructive →
// red. A tap-twice-armed reset swaps its LABEL to "really?" via a local items
// copy, so the shared list renderer can stay generic.
static void usageMeta(int i, char* out, size_t osz, uint16_t* col, bool* danger) {
  if (i == 0) { snprintf(out, osz, "%s", USAGE_SPANS[usageSpanIdx]); *col = DS_ORANGE; }
  else if (i == 2) { snprintf(out, osz, "!"); *col = DS_REDSOFT; *danger = true; }
}
static void drawUsage() {
  const char* items[USAGE_N];
  for (int i = 0; i < USAGE_N; i++) items[i] = usageItems[i];
  // Reflect an armed tap-twice confirm in the row label.
  bool armed = (int32_t)(millis() - usageConfirmUntil) < 0;
  if (armed && usageConfirmIdx == 1) items[1] = "really?";
  if (armed && usageConfirmIdx == 2) items[2] = "really?!";
  drawListMenu("USAGE", items, USAGE_N, usageSel, "A scroll", "B select", usageMeta);
}

static void applyUsage(uint8_t idx) {
  uint32_t now = millis();
  switch (idx) {
    case 0:  // span — cycle day/week/month/all; emit to daemon, update label.
      usageSpanIdx = (usageSpanIdx + 1) % 4;
      {
        char ex[28];
        snprintf(ex, sizeof(ex), "\"value\":\"%s\"", USAGE_SPANVAL[usageSpanIdx]);
        sendEvt("period", ex);
      }
      return;
    case 1:    // reset count — tap-twice confirm.
    case 2: {  // reset level — tap-twice confirm (destructive).
      bool armed = (usageConfirmIdx == idx) && (int32_t)(now - usageConfirmUntil) < 0;
      if (!armed) {
        usageConfirmIdx = idx;
        usageConfirmUntil = now + 3000;
        beep(1400, 60);
        return;
      }
      beep(800, 200);
      usageConfirmIdx = 0xFF;
      if (idx == 1) {
        statsResetCounters();
        sendEvt("token_reset", nullptr);
      } else {
        statsResetLevel();
        characterInvalidate();
        sendEvt("level_reset", nullptr);
      }
      return;
    }
    case 3:  // Back
      usageOpen = false;
      usageConfirmIdx = 0xFF;
      characterInvalidate();
      return;
  }
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Axp.PowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

// Main menu — ScreenMenu. The "demo" row (index 4) carries an on/off meta.
static void menuMeta(int i, char* out, size_t osz, uint16_t* col, bool* danger) {
  if (i == 4) {
    bool on = dataDemo();
    snprintf(out, osz, "%s", on ? "on" : "off");
    *col = on ? DS_GREEN : DS_DIM;
  }
}
void drawMenu() {
  drawListMenu("MENU", menuItems, MENU_N, menuSel, "A scroll", "B select", menuMeta);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }

// Portrait clock — small clock area at the bottom of the sprite,
// above the buddy. Composed atomically into the main sprite which
// is pushed each frame.
static void drawClockPortrait() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  paintedOrient = 0;
  spr.fillRect(0, 170, W, H - 170, p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(3); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 192);
  spr.setTextSize(1); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 215);
                                                          spr.drawString(dl, CX, 228);
  spr.setTextDatum(TL_DATUM);
}

// Landscape clock — direct-to-LCD with rotation set. Pet on the left
// (via petSpr double-buffer or direct-draw fallback), clock on the right.
static void drawClockLandscape() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;

  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Clock text redraws only when the second changes (1 Hz), or on
  // full repaint after a rotation flip.
  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    // Centered at x=180 — pet area 0..110, 20-px gap to text.
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 180, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 180, 72);
                                                                  M5.Lcd.drawString(wdl, 180, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet redraw: 15 fps double-buffered (smooth), 5 fps direct-draw fallback.
  // On a full repaint (orientation flip or screen-change entry) redraw the pet
  // this frame too, so it isn't briefly blank after the fillScreen() clear.
  static uint32_t lastPetTick = 0;
  uint32_t redrawIntervalMs = petSprReady ? 66 : 200;
  if (repaint || millis() - lastPetTick >= redrawIntervalMs) {
    lastPetTick = millis();
    if (buddyMode) {
      if (petSprReady) {
        petSpr.fillSprite(p.bg);
        buddyRenderTo(&petSpr, activeState);
        petSpr.pushSprite(0, 0);
      } else {
        M5.Lcd.fillRect(0, 0, 110, 135, p.bg);
        buddyRenderTo(&M5.Lcd, activeState);
        M5.Lcd.fillRect(0, 0, 32, 8, 0xF800);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(0xFFFF, 0xF800);
        M5.Lcd.setCursor(2, 1);
        M5.Lcd.print("flicker");
      }
    } else {
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
}

// Thin dispatcher — picks the right clock renderer based on detected
// device orientation. The two branches are independently understandable
// now that they live in separate functions.
static void drawClock() {
  if (clockOrient == 0) drawClockPortrait();
  else                  drawClockLandscape();
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// Shared approve/deny actions — called from BtnA, BtnB, and mic claps.
// `promptArrivedMs` and `responseSent` are file-scope globals;
// `inPrompt` is recomputed inline from `tama.promptId` to match how
// loop() already does it.
static void sendApprove() {
  if (!tama.promptId[0] || responseSent) return;
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
  sendCmd(cmd);
  responseSent = true;
  uint32_t tookS = (millis() - promptArrivedMs) / 1000;
  statsOnApproval(tookS);
  beep(2400, 60);
  if (tookS < 5) triggerOneShot(P_HEART, 2000);
}
static void sendDeny() {
  if (!tama.promptId[0] || responseSent) return;
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
  sendCmd(cmd);
  responseSent = true;
  statsOnDenial();
  beep(600, 60);
}

// Dismiss the currently-shown approval prompt without sending a decision.
// Used by the {"cmd":"clearprompt"} bridge command (see xfer.h) to clear a
// stale device prompt that was already resolved on the computer. Clears all
// prompt state in `tama` plus the file-scope arrival bookkeeping so the
// arrival detector in loop() doesn't immediately re-fire, and invalidates the
// character so the normal screen redraws. Lives here (rather than xfer.h)
// because the prompt state — `tama` (TamaState), `lastPromptId`,
// `responseSent` — is all defined in this translation unit.
void clearPromptState() {
  tama.promptId[0]   = 0;
  tama.promptTool[0] = 0;
  tama.promptHint[0] = 0;
  tama.promptSrc[0]  = 0;
  lastPromptId[0]    = 0;   // keep arrival detector in sync (matches "" now)
  responseSent       = false;
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Design-style info header strip (ScreenInfo): a full-width dark band with the
// section title at HEADER size and a dim "page+1/total" counter at SUB (the
// "3/7" caption stays size 1 by design), hairline dividers top and bottom.
// Sits below the small gr0m head peek (rows 0..TOP).
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  const int HEADER_H = 18;
  spr.fillRect(0, y, W, HEADER_H, DS_DARK);
  spr.drawFastHLine(0, y, W, DS_LINE);
  spr.drawFastHLine(0, y + HEADER_H, W, DS_LINE);
  spr.setTextSize(FS_HEADER);
  spr.setTextColor(DS_WHITE, DS_DARK);
  spr.setCursor(6, y + 2);
  spr.print(section);
  char pb[8]; snprintf(pb, sizeof(pb), "%u/%u", page + 1, INFO_PAGES);
  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_DIM, DS_DARK);
  spr.setCursor(W - 6 - (int)strlen(pb) * 6, y + 5);
  spr.print(pb);
  y += HEADER_H + 6;
  spr.setTextSize(FS_SUB);
}

// ScreenPasskey — BLUETOOTH PAIRING. Blue header banner, dim "ENTER ON DESKTOP"
// label, the 6-digit passkey inside a glowing cyan double-border group (size 3
// hero digits), then DEVICE NAME + the live advertised name.
void drawPasskey() {
  spr.fillSprite(DS_BLACK);

  // Header banner — blue, centered title (size 1, letter-spaced look).
  spr.fillRect(0, 0, W, 18, DS_PASSHDR);
  spr.setTextSize(1);
  spr.setTextColor(DS_WHITE, DS_PASSHDR);
  const char* hdr = "BT PAIRING";
  spr.setCursor((W - (int)strlen(hdr) * 6) / 2, 5);
  spr.print(hdr);

  // "ENTER ON DESKTOP" dim label.
  spr.setTextColor(DS_DIM, DS_BLACK);
  const char* sub = "ENTER ON DESKTOP";
  spr.setCursor((W - (int)strlen(sub) * 6) / 2, 56);
  spr.print(sub);

  // Cyan double-bordered digit group with the hero passkey (size 3).
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  const int digW = 18 * 6;            // 6 digits × 18 px at size 3
  const int dx   = (W - digW) / 2 - 6;
  const int dy   = 84;
  const int dw   = digW + 12;
  const int dh   = 34;
  spr.fillRoundRect(dx, dy, dw, dh, 4, DS_DARK);
  spr.drawRoundRect(dx,     dy,     dw,     dh,     4, DS_CYAN);
  spr.drawRoundRect(dx - 1, dy - 1, dw + 2, dh + 2, 5, DS_CYAN);
  spr.setTextSize(3);
  spr.setTextColor(DS_WHITE, DS_DARK);
  spr.setCursor((W - digW) / 2, dy + 6);
  spr.print(b);

  // DEVICE NAME + live advertised name.
  spr.setTextSize(1);
  spr.setTextColor(DS_DIM, DS_BLACK);
  const char* dnl = "DEVICE NAME";
  spr.setCursor((W - (int)strlen(dnl) * 6) / 2, 152);
  spr.print(dnl);
  spr.setTextSize(2);
  spr.setTextColor(DS_WHITE, DS_BLACK);
  spr.setCursor((W - (int)strlen(btName) * 12) / 2, 166);
  spr.print(btName);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  // ln() — size 1 (data pages: CLAUDE / DEVICE / BLUETOOTH kv rows fit).
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setTextSize(1);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };
  // lg() — TFT_eSPI built-in Font 2 (8×16 proportional, ~16 chars/line).
  // Bigger than the default size-1 font but slimmer than setTextSize(2)
  // so more text fits per line while still being legible. Resets to
  // Font 1 on exit so callers that assume default+setTextSize still work.
  auto lg = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setTextFont(2);
    spr.setTextSize(1);
    spr.setCursor(4, y); spr.print(b); y += 16;
    spr.setTextFont(1);
  };

  if (infoPage == 0) {
    // ScreenInfo — ABOUT. White lead line (size 2), hairline, dim copy.
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(DS_WHITE, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);       spr.print("I watch");
    spr.setCursor(4, y + 18);  spr.print("your Claude");
    spr.setCursor(4, y + 36);  spr.print("sessions.");
    y += 60;
    spr.drawFastHLine(4, y, W - 8, DS_LINE);
    y += 8;
    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(4, y);       spr.print("Sleep when");
    spr.setCursor(4, y + 18);  spr.print("idle. Wake");
    spr.setCursor(4, y + 36);  spr.print("on prompts.");
    spr.setTextSize(1);

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    lg("A  front");
    spr.setTextColor(p.textDim, p.bg); lg("   next / approve");
    spr.setTextColor(p.text, p.bg);    lg("B  right side");
    spr.setTextColor(p.textDim, p.bg); lg("   page / deny");
    spr.setTextColor(p.text, p.bg);    lg("hold A");
    spr.setTextColor(p.textDim, p.bg); lg("   open menu");
    spr.setTextColor(p.text, p.bg);    lg("Power  left");
    spr.setTextColor(p.textDim, p.bg); lg("   6s = power off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    // Wide kv rows use Font 2 (8×16) — the body floor for data rows — instead
    // of the old size-1 (10px) sub-text that read too small at arm's length.
    // Read live every frame so the LINK block reflects the radio state now.
    spr.setTextColor(p.textDim, p.bg);
    // The bridge is a prompt gateway, not session-aware, so it can't report
    // live Claude session counts (they were always 0). Show what it DOES track
    // accurately and pushes every heartbeat: approvals/denials this period.
    lg("approved %u", stats().okCount);
    lg("denied   %u", stats().noCount);
    y += 4;
    spr.setTextColor(DS_DIM, p.bg);
    spr.setTextSize(FS_SUB);
    spr.setCursor(4, y); spr.print("LINK"); y += 10;
    spr.setTextColor(p.textDim, p.bg);
    // Live radio read at draw time: BT (connected / suspended-by-WiFi / off)
    // and the active transport name.
    const char* bleLbl = bleSuspended() ? "wifi owns" : !bleConnected() ? "-"
                       : bleSecure() ? "encrypted" : "OPEN";
    lg("via  %s", dataScenarioName());
    lg("ble  %s", bleLbl);
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    lg("last %lus", (unsigned long)age);
    lg("st   %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat_mA = (int)M5.Axp.GetBatCurrent();
    int vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", bleSuspended() ? "wifi owns"
                       : settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
    // ScreenLinks — the live connection status page. Hero BT link state
    // (LINKED / READY / SUSPEND / OFF) as the one big word, an ENCRYPTED status
    // dot, the device name at BODY size (NOT a hero — it's too long to read big
    // and it isn't the headline), then live WiFi (STA IP / AP clients) + VPN.
    // EVERYTHING here is read at DRAW TIME each frame: drawInfo() re-runs every
    // loop while DISP_INFO is active, so toggling a radio or (dis)connecting a
    // peer is reflected immediately without any cached snapshot.
    _infoHeader(p, y, "LINKS", infoPage);

    bool suspended = bleSuspended();          // BLE suspended → WiFi owns radio
    bool connected = bleConnected();
    bool linked    = settings().bt && !suspended && (connected || dataBtActive());
    bool secure    = connected && bleSecure();
    // Hero link WORD — FS_HERO (size 3). Green linked / amber ready / dim off,
    // and a distinct "SUSPEND" when the WiFi/BLE radio mutex has parked BLE.
    const char* big = suspended ? "SUSPEND"
                    : linked     ? "LINKED"
                    : settings().bt ? "READY" : "OFF";
    uint16_t bigCol = suspended ? DS_DIM
                    : linked    ? DS_GREEN
                    : settings().bt ? DS_AMBER : DS_DIM;
    spr.setTextSize(FS_HERO);
    spr.setTextColor(bigCol, p.bg);
    spr.setCursor(4, y);
    spr.print(big);
    y += 28;
    // ENCRYPTED status dot + label (sub caption beside the hero state).
    if (linked) {
      uint16_t encCol = secure ? DS_GREEN : DS_AMBER;
      drawStatusDot(8, y + 4, encCol, 3, true);
      spr.setTextSize(FS_SUB);
      spr.setTextColor(encCol, p.bg);
      spr.setCursor(16, y + 1);
      spr.print(secure ? "ENCRYPTED" : "OPEN LINK");
      y += 14;
    } else if (suspended) {
      spr.setTextSize(FS_SUB);
      spr.setTextColor(DS_DIM, p.bg);
      spr.setCursor(4, y + 1);
      spr.print("WiFi owns radio");
      y += 14;
    }
    spr.drawFastHLine(4, y, W - 8, DS_LINE);
    y += 6;
    // DEVICE — caption (size 1) then the name at BODY (size 2). The name is
    // deliberately NOT a hero: "Claude-XXXX" is 11 chars and reads as data.
    spr.setTextSize(FS_SUB);
    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(4, y); spr.print("DEVICE");
    y += 10;
    spr.setTextSize(FS_BODY);
    spr.setTextColor(DS_WHITE, p.bg);
    spr.setCursor(4, y); spr.print(btName);
    y += 20;
    // WiFi — live STA state read each frame. ONLINE shows the current IP; the
    // setup-AP state shows the live count of associated stations.
    NetWifiState ws = netWifiState();
    if (ws != NW_OFF) {
      uint16_t wc = (ws == NW_ONLINE) ? DS_GREEN : (ws == NW_FAILED) ? DS_RED : DS_AMBER;
      const char* wl = (ws == NW_ONLINE) ? "online" : (ws == NW_PORTAL) ? "setup AP"
                     : (ws == NW_FAILED) ? "failed" : "connecting";
      spr.setTextSize(FS_SUB);
      spr.setTextColor(DS_DIM, p.bg); spr.setCursor(4, y); spr.print("WIFI");
      spr.setTextColor(wc, p.bg); spr.setCursor(40, y); spr.print(wl);
      y += 10;
      if (ws == NW_ONLINE) {
        spr.setTextColor(DS_DIM, p.bg); spr.setCursor(4, y);
        spr.print(netWifiIP());
        y += 10;
      } else if (ws == NW_PORTAL) {
        spr.setTextColor(DS_DIM, p.bg); spr.setCursor(4, y);
        spr.printf("clients: %d", netWifiApClients());
        y += 10;
      }
    }
    if (netWgState() != WG_OFF) {
      NetWgState gs = netWgState();
      uint16_t gc = gs == WG_UP ? DS_GREEN : gs == WG_FAILED ? DS_RED : DS_AMBER;
      spr.setTextSize(FS_SUB);
      spr.setTextColor(DS_DIM, p.bg); spr.setCursor(4, y); spr.print("VPN");
      spr.setTextColor(gc, p.bg); spr.setCursor(40, y);
      spr.print(gs == WG_UP ? "up" : gs == WG_FAILED ? "failed" : "...");
      y += 10;
    }
    // Hint: BtnA long-press opens the Connection submenu (gated in loop()).
    spr.setTextColor(DS_DIM, p.bg);
    spr.setTextSize(FS_SUB);
    spr.setCursor(4, H - 10); spr.print("hold A: edit");

  } else if (infoPage == INFO_PG_USAGE) {
    // ScreenUsage — TODAY hero + OK/NO + EVOLUTION stage + progress bar + LEVEL.
    _infoHeader(p, y, "USAGE", infoPage);

    // TODAY hero (period tokens, orange, short format) — size 3 number.
    spr.setTextColor(DS_DIM, p.bg);
    spr.setTextSize(1);
    spr.setCursor(4, y); spr.print("TODAY");
    y += 10;
    char tb[12]; char unit = 0;
    fmtTokens(tb, sizeof(tb), stats().tokens, &unit);
    spr.setTextSize(3);
    spr.setTextColor(DS_ORANGE, p.bg);
    spr.setCursor(4, y); spr.print(tb);
    if (unit) {
      int w3 = (int)strlen(tb) * 18;
      spr.setTextSize(2);
      spr.setCursor(4 + w3 + 2, y + 7);
      spr.print(unit);
    }
    y += 30;

    // OK / NO counters (size 2 values).
    spr.setTextSize(1);
    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(4, y);  spr.print("OK");
    spr.setCursor(70, y); spr.print("NO");
    spr.setTextSize(2);
    spr.setTextColor(DS_GREEN, p.bg);  spr.setCursor(4, y + 10);  spr.printf("%u", stats().okCount);
    spr.setTextColor(DS_REDSOFT, p.bg); spr.setCursor(70, y + 10); spr.printf("%u", stats().noCount);
    y += 32;
    spr.drawFastHLine(4, y, W - 8, DS_LINE);
    y += 6;

    // EVOLUTION — "Stage n/5" + next-milestone arrow, then a progress bar.
    uint8_t stg = evoStage();
    uint32_t nxt = evoNextMilestone();
    spr.setTextSize(1);
    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(4, y); spr.print("EVOLUTION");
    // milestone label, right-aligned
    char mb[10];
    if (nxt == 0)              snprintf(mb, sizeof(mb), "MAX");
    else if (nxt >= 1000000UL) snprintf(mb, sizeof(mb), ">%luM", (unsigned long)(nxt / 1000000UL));
    else                       snprintf(mb, sizeof(mb), ">%luK", (unsigned long)(nxt / 1000UL));
    spr.setTextColor(DS_ORANGE, p.bg);
    spr.setCursor(W - 4 - (int)strlen(mb) * 6, y); spr.print(mb);
    y += 11;
    spr.setTextSize(2);
    spr.setTextColor(DS_WHITE, p.bg);
    spr.setCursor(4, y); spr.printf("Stage %u", stg);
    spr.setTextSize(1);
    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(4 + 7 * 12 + 2, y + 8); spr.print("/5");
    y += 20;
    // Progress bar — fraction of the way to the next milestone.
    {
      uint32_t life = stats().lifetimeTokens;
      uint32_t prev = (stg == 0) ? 0 : EVO_MILESTONES[stg - 1];
      uint32_t span = (nxt == 0) ? 1 : (nxt - prev);
      uint32_t into = (life > prev) ? (life - prev) : 0;
      int pct = (nxt == 0) ? 100 : (int)((into * 100) / span);
      if (pct > 100) pct = 100;
      int bw = W - 8;
      spr.fillRect(4, y, bw, 6, DS_LINE);
      int fw = (bw * pct) / 100;
      if (fw > 0) spr.fillRect(4, y, fw, 6, DS_ORANGE);
    }
    y += 12;
    // LEVEL row.
    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(4, y); spr.print("LEVEL");
    spr.setTextColor(DS_WHITE, p.bg);
    char lvb[8]; snprintf(lvb, sizeof(lvb), "%u", stats().level);
    spr.setCursor(W - 4 - (int)strlen(lvb) * 6, y); spr.print(lvb);
    y += 12;

    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(4, H - 10); spr.print("hold A: edit");

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    lg("made by");
    spr.setTextColor(p.text, p.bg);
    lg("Michael");
    lg("Groberman");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    lg("upstream");
    spr.setTextColor(p.text, p.bg);
    lg("anthropic /");
    lg("claude-buddy");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    lg("hardware");
    spr.setTextColor(p.text, p.bg);
    lg("M5StickC+");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// drawApproval — redesigned permission prompt.
// Takes the bottom 132 px (out of 240) for a bigger, scannable layout:
//   • red alarm bar with elapsed timer
//   • HUGE tool name (size 3 = 24 px glyphs)
//   • truncated command preview underneath
//   • full-width ALLOW (green) / DENY (red) action zones at the bottom
// The buddy keeps drawing in the upper region (attention mood: red visor,
// LED blink) so the urgency is doubled by the chest-LED firmware path.
static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 132;
  const int TOP = H - AREA;     // 240 - 132 = 108
  const uint16_t ALARM   = 0xF800;   // red
  const uint16_t ALLOW_BG = 0x0A20;  // dark green
  const uint16_t ALLOW_FG = 0x07E0;  // bright green
  const uint16_t DENY_BG  = 0x3000;  // dark red
  const uint16_t DENY_FG  = 0xFA20;

  // Background (covers buddy clipping below TOP)
  spr.fillRect(0, TOP, W, AREA, p.bg);

  // ── ALARM BAR ───────────────────────────────────────────────────
  // Red on arrival; flips to amber once the request has been pending >10 s so
  // a glance at the bar colour (not just the timer digits) signals staleness.
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  const uint16_t AMBER = 0xFD20;          // orange-amber for the "stale" bar
  uint16_t barCol = (waited >= 10) ? AMBER : ALARM;
  spr.fillRect(0, TOP, W, 22, barCol);
  spr.setTextSize(2);
  spr.setTextColor(0x0000, barCol);
  spr.setCursor(6, TOP + 4);
  spr.print("APPROVE");
  // Source badge — bridge tags prompts with "cli" / "app" / "mob".
  // Sits to the right of APPROVE in size 1 so you can tell at a glance
  // whether the prompt came from Claude Code, the desktop app, etc.
  if (tama.promptSrc[0]) {
    spr.setTextSize(1);
    spr.setTextColor(0xFFFF, barCol);
    spr.setCursor(6 + 7 * 12 + 4, TOP + 8);
    spr.print(tama.promptSrc);
  }
  // elapsed time, right-aligned, black on the bar (the bar itself carries the
  // colour state now, so the digits stay high-contrast at every age).
  spr.setTextSize(2);
  char tb[8]; snprintf(tb, sizeof(tb), "%lus", (unsigned long)waited);
  int tlen = strlen(tb);
  spr.setTextColor(0x0000, barCol);
  spr.setCursor(W - tlen * 12 - 6, TOP + 4);
  spr.print(tb);

  // ── TOOL NAME (huge) ─────────────────────────────────────────────
  spr.setTextSize(1);
  spr.setTextColor(HOT, p.bg);
  spr.setCursor(6, TOP + 28);
  spr.print("TOOL");

  int toolLen = strlen(tama.promptTool);
  // Hero the tool name: size 3 (18px glyph, ~7 chars across 135 wide) is the
  // design target ("Bash" fills the panel). Step down to 2 then 1 only when
  // the name is too long to fit, so common tools stay huge.
  uint8_t toolSize = (toolLen <= 7) ? 3 : (toolLen <= 10) ? 2 : 1;
  spr.setTextSize(toolSize);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, TOP + 38);
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // ── COMMAND PREVIEW ──────────────────────────────────────────────
  // "RUN" label, then the command on the next line(s). The footer at the very
  // bottom is drawn afterward and paints over any overrun, so the line budget
  // here is sized to the gap between the tool name and the footer:
  //   size 3 tool → 1 preview line, size 2/1 → up to 2 lines.
  int previewY = TOP + 38 + (toolSize == 3 ? 26 : toolSize == 2 ? 18 : 12);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, previewY);
  spr.print("RUN");
  spr.setTextColor(p.text, p.bg);
  // Command preview in Font 2 (8px proportional). ~16 chars fit across the
  // 135 px screen per line; hard-truncate to the available line budget so the
  // text never spills past the action footer.
  spr.setTextFont(2);
  spr.setTextSize(1);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(6, previewY + 12);
  spr.printf("%.16s", tama.promptHint);
  if (toolSize != 3 && hlen > 16) {
    spr.setCursor(6, previewY + 28);
    spr.printf("%.16s", tama.promptHint + 16);
  }
  spr.setTextFont(1);

  // ── ACTION FOOTER — ALLOW | DENY ────────────────────────────────
  const int FH = 36;       // footer height
  const int FY = H - FH;
  spr.fillRect(0,       FY, W / 2,     FH, responseSent ? PANEL : ALLOW_BG);
  spr.fillRect(W / 2,   FY, W - W/2,   FH, responseSent ? PANEL : DENY_BG);
  spr.drawFastVLine(W / 2, FY, FH, 0x0000);
  spr.drawFastHLine(0, FY, W, 0x0000);

  if (responseSent) {
    spr.setTextSize(2);
    spr.setTextColor(p.textDim, PANEL);
    spr.setCursor((W - 6 * 12) / 2, FY + 10);
    spr.print("sent…");
    spr.setTextSize(1);
  } else {
    // ALLOW: ✓ + label
    spr.setTextSize(3);
    spr.setTextColor(ALLOW_FG, ALLOW_BG);
    spr.setCursor(20, FY + 4);
    spr.print("OK");
    spr.setTextSize(1);
    spr.setTextColor(ALLOW_FG, ALLOW_BG);
    spr.setCursor(16, FY + 28);
    spr.print("A allow");
    // DENY: ✗ + label
    spr.setTextSize(3);
    spr.setTextColor(DENY_FG, DENY_BG);
    spr.setCursor(W / 2 + 20, FY + 4);
    spr.print("NO");
    spr.setTextSize(1);
    spr.setTextColor(DENY_FG, DENY_BG);
    spr.setCursor(W / 2 + 18, FY + 28);
    spr.print("B deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

// drawPetStats — redesigned card-style stats page.
// Big LV pill + Fed/Energy bars at top, 2×2 grid below with hero numbers
// in mood-colored typography. Header on top is drawn by drawPet() —
// content starts at y=88 to leave 16 px for the title row.
static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);

  // ── LV CHIP ─────────────────────────────────────────────────────
  // Orange pill chip per the design, with black text for contrast. The chip
  // colour is the FIXED design-system orange (DS_ORANGE, #ff8c1a) — the same
  // accent the list-menu selection and USAGE hero use — not the per-character
  // body tint, so the level badge reads the same across every buddy palette.
  int y = 88;
  spr.fillRoundRect(6, y, 46, 18, 3, DS_ORANGE);
  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_BLACK, DS_ORANGE);
  spr.setCursor(11, y + 5);
  spr.print("LV");
  spr.setTextSize(FS_BODY);
  spr.setTextColor(DS_BLACK, DS_ORANGE);
  spr.setCursor(26, y + 2);
  spr.printf("%u", stats().level);
  spr.setTextSize(FS_SUB);

  // ── FED METER ───────────────────────────────────────────────────
  // Label is a fixed dim caption (DS_DIM) beside a body-size value — the same
  // label/value treatment the USAGE info page uses, so the eye reads it the
  // same here. The value + dot fill stay on the per-character body tint.
  y = 112;
  spr.setTextColor(DS_DIM, p.bg);
  spr.setCursor(6, y);
  spr.print("FED");
  uint8_t fed = statsFedProgress();
  spr.setTextColor(p.body, p.bg);
  // Dynamically right-align so "10/10" (5 chars × 6 px = 30) doesn't run
  // off the 135-px screen — the patch used a fixed W-22 that overflowed.
  char fb[8]; snprintf(fb, sizeof(fb), "%u/10", fed);
  spr.setCursor(W - 4 - (int)strlen(fb) * 6, y);
  spr.print(fb);
  for (int i = 0; i < 10; i++) {
    int px = 6 + i * 12;
    if (i < fed) spr.fillCircle(px + 4, y + 16, 4, p.body);
    else         spr.drawCircle(px + 4, y + 16, 4, p.textDim);
  }

  // ── BATTERY ─────────────────────────────────────────────────────
  // Real hardware battery (AXP192), replacing the prior pet "energy" tier.
  y = 140;
  int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
  int iBat_mA = (int)M5.Axp.GetBatCurrent();
  int vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
  int pct = (vBat_mV - 3200) / 10;    // (v-3.2)/(4.2-3.2)*100 with mV
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  bool usb      = vBus_mV > 4000;
  bool charging = usb && iBat_mA > 1;
  // Fixed design-system status accents (green ok / amber mid / red low) so the
  // battery reads the same colour story as the header battery glyph and the
  // DEVICE/USAGE pages, independent of the buddy skin.
  uint16_t batCol = (pct >= 50) ? DS_GREEN : (pct >= 20) ? DS_AMBER : DS_REDSOFT;

  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_DIM, p.bg);
  spr.setCursor(6, y);
  spr.print("BATTERY");
  // Mode badge on the right of the label row — right-aligned with 4 px margin.
  const char* badge = charging ? "CHG" : (usb ? "USB" : nullptr);
  if (badge) {
    spr.setTextColor(charging ? DS_AMBER : DS_CYAN, p.bg);
    spr.setCursor(W - 4 - (int)strlen(badge) * 6, y);
    spr.print(badge);
  }
  // Percentage in batt color — fits between BATTERY label (ends ~x=48) and
  // the badge (starts ~x=113 at most). "100%" is 4 chars * 6 = 24 px wide.
  spr.setTextColor(batCol, p.bg);
  spr.setCursor(56, y);
  spr.printf("%d%%", pct);

  // Battery shell with terminal nub + fill
  int by = y + 12;
  int bw = W - 18;     // leave room for the terminal on the right
  int bh = 12;
  spr.drawRect(6, by, bw, bh, p.textDim);
  spr.fillRect(6 + bw, by + 3, 4, bh - 6, p.textDim);  // terminal nub
  int fill = (pct * (bw - 4)) / 100;
  if (fill > 0) spr.fillRect(8, by + 2, fill, bh - 4, batCol);

  // ── DIVIDER ─────────────────────────────────────────────────────
  y = 170;
  spr.drawFastHLine(6, y, W - 12, p.textDim);

  // ── 2×2 STAT GRID — APPROVED / DENIED / TOKENS / NAPPED ─────────
  y = 178;
  const int COL2 = 72;
  // Each cell: a fixed dim caption (DS_DIM) over a body-size value in its
  // fixed status accent — same label/value pattern as the USAGE OK/NO block.
  auto cell = [&](int cx, int cy, const char* label, uint16_t labelCol, uint16_t valCol, const char* fmt, uint32_t v) {
    spr.setTextSize(FS_SUB);
    spr.setTextColor(labelCol, p.bg);
    spr.setCursor(cx, cy);
    spr.print(label);
    spr.setTextSize(FS_BODY);
    spr.setTextColor(valCol, p.bg);
    spr.setCursor(cx, cy + 10);
    spr.printf(fmt, (unsigned long)v);
    spr.setTextSize(FS_SUB);
  };

  // Token short formatter — returns char* in caller-owned buffer
  auto tokStr = [](char* buf, size_t bsz, uint32_t v) -> const char* {
    if      (v >= 1000000000UL) snprintf(buf, bsz, "%lu.%luB", v / 1000000000UL, (v / 100000000UL) % 10);
    else if (v >= 10000000UL)   snprintf(buf, bsz, "%luM", v / 1000000UL);                       // 42M
    else if (v >= 1000000UL)    snprintf(buf, bsz, "%lu.%luM", v / 1000000UL, (v / 100000UL) % 10); // 3.7M
    else if (v >= 1000UL)       snprintf(buf, bsz, "%luK", v / 1000UL);                           // 679K (no decimal)
    else                        snprintf(buf, bsz, "%lu", (unsigned long)v);
    return buf;
  };

  // Row 1
  cell(6,    y, "APPROVED", DS_DIM, DS_GREEN,   "%lu", stats().approvals);
  cell(COL2, y, "DENIED",   DS_DIM, DS_REDSOFT, "%lu", stats().denials);

  // Row 2 — TOKENS, NAPPED
  y += 32;
  char tbuf[12];
  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_DIM, p.bg);
  spr.setCursor(6, y);
  spr.print("TOKENS");
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(FS_BODY);
  spr.setCursor(6, y + 10);
  spr.print(tokStr(tbuf, sizeof(tbuf), stats().tokens));
  spr.setTextSize(FS_SUB);

  uint32_t nap = stats().napSeconds;
  spr.setTextColor(DS_DIM, p.bg);
  spr.setCursor(COL2, y);
  spr.print("NAPPED");
  spr.setTextSize(FS_BODY);
  spr.setTextColor(DS_CYAN, p.bg);
  spr.setCursor(COL2, y + 10);
  spr.printf("%luh%02lu", nap / 3600, (nap / 60) % 60);
  spr.setTextSize(FS_SUB);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  int y = TOP + 18;            // clear the PET header strip drawn by drawPet()
  // Same Font 2 style the Info paragraph pages use — 8×16 px proportional,
  // ~16 chars/line on a 135 px screen, pitch 14 leaves a touch of air.
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg);
    spr.setTextFont(2); spr.setTextSize(1);
    spr.setCursor(6, y); spr.print(s); y += 14;
    spr.setTextFont(1);        // restore default
  };
  auto gap = [&]() { y += 4; };

  ln(p.body,    "MOOD");
  ln(p.textDim, " quick approve");
  ln(p.textDim, " keeps it up");      gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up");         gap();

  ln(p.body,    "BATTERY");
  ln(p.textDim, " usb charges");      gap();

  ln(p.textDim, "A: page");
  ln(p.textDim, "hold A: menu");
}

// Evolution showcase overlay (Pet page PET_PG_SHOWCASE). The character itself
// is rendered behind us (peeked) with g_evoStage transiently set to the cycling
// showcase stage in loop(), so the body / antenna / shades / HUD / disguise pop
// in and out as the demo walks 0..5. We only draw chrome: a compact bottom band
// with a "Stage N/5" pill, the stage name, and a 6-segment progress strip. The
// upper region is left untouched so the showing-off character is fully visible.
static void drawPetShowcase(const Palette& p) {
  uint8_t s = showcaseStage();          // 0..5, matches the overridden g_evoStage

  // Bottom band only — keep the character region clear.
  const int BAND_H = 46;
  const int by = H - BAND_H;
  spr.fillRect(0, by, W, BAND_H, p.bg);
  spr.drawFastHLine(0, by, W, p.textDim);

  // "Stage N/5" pill (left) + stage name (right).
  int ty = by + 6;
  spr.fillRoundRect(4, ty, 56, 16, 3, p.body);
  spr.setTextSize(1);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(9, ty + 4);
  spr.printf("Stage %u/5", s);

  spr.setTextColor(p.text, p.bg);
  const char* name = EVO_STAGE_NAMES[s <= 5 ? s : 5];
  spr.setCursor(W - 4 - (int)strlen(name) * 6, ty + 4);
  spr.print(name);

  // 6-segment progress strip: filled = stages reached so far this cycle, the
  // current one highlighted, the rest dim (a "still to come" preview).
  int sy = by + 28;
  int segW = (W - 8) / 6;
  for (uint8_t i = 0; i <= 5; i++) {
    int sx = 4 + i * segW;
    if (i == s)      spr.fillRect(sx, sy, segW - 2, 10, p.body);
    else if (i < s)  spr.fillRect(sx, sy, segW - 2, 10, p.text);
    else             spr.drawRect(sx, sy, segW - 2, 10, p.textDim);
  }
}

// Pet header strip — the same dark band + hairlines + FS_HEADER title look the
// Info pages get from _infoHeader, so PET reads as part of the redesigned set
// (canvas screen 4: dark header, pet/owner title, page counter) instead of the
// old bare floating title. Sits in the band just under the peeked character
// (rows 0..70) and above the page body (LV chip / how-to copy start at y=88).
static void drawPetHeader(const Palette& p) {
  const int HY = 70, HH = 16;
  spr.fillRect(0, HY, W, HH, DS_DARK);
  spr.drawFastHLine(0, HY, W, DS_LINE);
  spr.drawFastHLine(0, HY + HH, W, DS_LINE);

  // Title = pet name, or "owner's pet" when an owner is set and differs. Size 2
  // (HEADER) fits ~10 chars; longer owner names step down to the SUB caption
  // size so they don't wrap out of the 135 px strip — same rule as before.
  char title[40];
  const char* owner = ownerName();
  if (owner[0] && strcasecmp(owner, petName()) != 0) {
    snprintf(title, sizeof(title), "%s's %s", owner, petName());
  } else {
    snprintf(title, sizeof(title), "%s", petName());
  }
  bool small = strlen(title) > 10;
  spr.setTextSize(small ? FS_SUB : FS_HEADER);
  spr.setTextColor(DS_WHITE, DS_DARK);
  // size-2 title (16 px) sits flush at the band top so it stays inside the
  // 16 px strip; the size-1 fallback is centred.
  spr.setCursor(6, small ? HY + 4 : HY);
  spr.print(title);

  // Page counter at SUB, right-aligned — matches the "n/total" caption the
  // Info / list headers carry.
  char pb[8]; snprintf(pb, sizeof(pb), "%u/%u", petPage + 1, PET_PAGES);
  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_DIM, DS_DARK);
  spr.setCursor(W - 6 - (int)strlen(pb) * 6, HY + 4);
  spr.print(pb);
}

void drawPet() {
  const Palette& p = characterPalette();

  // The showcase page draws only a bottom chrome band over the peeked,
  // stage-cycling character — no full-screen page, no top title row (which
  // would otherwise cover the character's head as it shows off each form).
  if (petPage == PET_PG_SHOWCASE) { drawPetShowcase(p); return; }

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header strip on top of whichever page drew.
  drawPetHeader(p);
}

// WiFi status block (top-right corner). Shows whenever the WiFi
// setting is enabled, with state-dependent color + label:
//   STARTING   → yellow "WIFI..."
//   PORTAL     → orange "AP"
//   CONNECTING → cyan animated dots
//   ONLINE     → green WIFI bars + RSSI
//   FAILED     → red "WIFI X"
//   OFF        → nothing (setting disabled)
static void drawWifiIndicator() {
  if (!settings().wifi) return;
  const int bw = 28, bh = 12;
  const int bx = W - bw - 1;
  const int by = 1;
  NetWifiState st = netWifiState();
  uint16_t fg, bg = 0x18C3;
  const char* lbl = "";
  switch (st) {
    case NW_OFF:        return;
    case NW_STARTING:   fg = 0xFFE0; lbl = "..."; break;
    case NW_PORTAL:     fg = 0xFD20; lbl = "AP"; break;
    case NW_CONNECTING: fg = 0x07FF; lbl = "..."; break;
    case NW_ONLINE: {
      // Static "online" badge. No RSSI polling — querying signal strength
      // wakes the radio and costs battery/CPU every frame, so we just show
      // a fixed connected glyph instead of live bars.
      spr.fillRect(bx, by, bw, bh, bg);
      spr.drawRect(bx, by, bw, bh, 0x07E0);
      uint16_t lit = 0x07E0;
      int sx = bx + 4;
      spr.fillRect(sx,     by + 7, 2, 3, lit);
      spr.fillRect(sx + 4, by + 4, 2, 6, lit);
      spr.fillRect(sx + 8, by + 2, 2, 8, lit);
      spr.setTextColor(lit, bg);
      spr.setTextSize(1);
      spr.setCursor(bx + 16, by + 3);
      spr.print("ON");
      return;
    }
    case NW_FAILED:     fg = 0xF800; lbl = "ERR"; break;
  }
  spr.fillRect(bx, by, bw, bh, bg);
  spr.drawRect(bx, by, bw, bh, fg);
  spr.setTextColor(fg, bg);
  spr.setTextSize(1);
  spr.setCursor(bx + 4, by + 3);
  spr.print(lbl);
}

// Full-width banner — WiFi setup info OR failure diagnostic. Sits
// just below the buddy area, above the HUD/transcript.
static void drawWifiPortalBanner() {
  NetWifiState st = netWifiState();
  if (st != NW_PORTAL && st != NW_FAILED) return;
  const int by = 124;
  const int bh = 46;
  uint16_t accent = (st == NW_FAILED) ? 0xF800 : 0xFD20;
  spr.fillRect(0, by, W, bh, 0x18C3);
  spr.drawFastHLine(0, by, W, accent);
  spr.drawFastHLine(0, by + bh - 1, W, accent);
  spr.setTextColor(accent, 0x18C3);
  spr.setTextSize(1);
  spr.setCursor(6, by + 3);
  if (st == NW_FAILED) {
    spr.print("WIFI FAILED");
    spr.setTextColor(0xFFFF, 0x18C3);
    spr.setCursor(6, by + 14);
    spr.print(netWifiLastError());
    spr.setCursor(6, by + 24);
    spr.print("toggle wifi off+on");
    spr.setCursor(6, by + 34);
    spr.print("to retry");
  } else {
    spr.print("WIFI SETUP");
    spr.setTextColor(0xFFFF, 0x18C3);
    spr.setCursor(6, by + 14);
    spr.print("ssid: gr0m-setup");
    spr.setCursor(6, by + 24);
    spr.print("pwd:  gr0mgr0m");
    spr.setCursor(6, by + 34);
    spr.print("url:  192.168.4.1");
  }
}

// Knock-listening indicator (top-right corner). State machine in
// precedence order: outcome flash → in-window count → armed → idle.
// Always renders a live audio-level meter below when mic is enabled.
static void drawListeningIndicator() {
  if (!micEnabled()) return;
  const int ix = W - 10;
  const int iy = 9;
  uint32_t now = millis();
  bool armed = tama.promptId[0] && !responseSent;
  uint32_t actionMs = micLastActionMs();
  uint8_t  action   = micLastAction();
  bool justFired = actionMs != 0 && (now - actionMs) < 800;
  uint8_t count = micCurrentCount();

  if (justFired) {
    uint16_t bg = (action == 1) ? 0x07E0 : 0xF800;  // green / red
    spr.fillCircle(ix, iy, 7, bg);
    spr.drawCircle(ix, iy, 8, 0xFFFF);
    if (action == 1) {
      spr.drawLine(ix - 4, iy + 0, ix - 1, iy + 3, 0x0000);
      spr.drawLine(ix - 3, iy + 0, ix - 1, iy + 2, 0x0000);
      spr.drawLine(ix - 1, iy + 3, ix + 4, iy - 3, 0x0000);
      spr.drawLine(ix - 1, iy + 2, ix + 3, iy - 3, 0x0000);
    } else {
      spr.drawLine(ix - 4, iy - 4, ix + 4, iy + 4, 0x0000);
      spr.drawLine(ix - 4, iy + 4, ix + 4, iy - 4, 0x0000);
      spr.drawLine(ix - 3, iy - 4, ix + 4, iy + 3, 0x0000);
      spr.drawLine(ix - 3, iy + 4, ix + 4, iy - 3, 0x0000);
    }
  } else if (count > 0) {
    spr.fillCircle(ix, iy, 7, 0xFFE0);
    spr.drawCircle(ix, iy, 8, 0xFFFF);
    spr.setTextColor(0x0000, 0xFFE0);
    spr.setTextSize(1);
    spr.setCursor(ix - 2, iy - 3);
    if (count < 10) spr.print((int)count);
    else            spr.print('+');
    uint32_t end = micWindowEndMs();
    int barW = 0;
    if (end > now) barW = ((end - now) * 14) / 600;
    if (barW > 14) barW = 14;
    spr.drawRect(ix - 7, iy + 11, 14, 3, 0xFFFF);
    if (barW > 0) spr.fillRect(ix - 7 + (14 - barW)/2, iy + 11, barW, 3, 0xFFE0);
  } else if (armed) {
    uint16_t color = ((now / 250) & 1) ? 0xF800 : 0x4000;
    spr.fillCircle(ix, iy, 3, color);
    spr.drawCircle(ix, iy, 4, 0xFFFF);
  } else {
    spr.fillCircle(ix, iy, 2, 0x4208);
  }

  // Live audio-level meter — width scales with peak relative to the
  // fire threshold (baseline × 3). Color shifts as the bar fills.
  int peak = micPeak();
  int base = micBaseline();
  int fireLevel = base * 3;
  if (fireLevel < 1500) fireLevel = 1500;
  int meterMax = 14;
  int meterW = (peak * meterMax) / fireLevel;
  if (meterW > meterMax) meterW = meterMax;
  if (meterW < 0)        meterW = 0;
  spr.drawRect(ix - 7, iy + 16, meterMax, 3, 0x4208);
  if (meterW > 0) {
    uint16_t mcolor = (peak > fireLevel) ? 0xFFE0
                   : (peak > fireLevel * 2 / 3) ? 0xFD20
                                                : 0x07E0;
    spr.fillRect(ix - 7, iy + 16, meterW, 3, mcolor);
  }
}

// ── HOME screen (ScreenHomeIdle / ScreenHomeWorking) ──────────────────────
// The buddy/character is already composited into the upper band of `spr` by
// buddyTick()/characterTick() before this runs. drawHUD overlays:
//   • the design header strip across the top 22 px (gr0m + state dot + battery)
//   • a bottom stats region driven by session state:
//       IDLE → "TOKENS TODAY" + a size-3 hero of g_dispTokens
//       BUSY → a RUN/WAIT/TOK inset strip + the two most-recent transcript rows
// Approval still short-circuits to drawApproval(). Transcript scroll + the
// response-preview banner behaviour are preserved in the BUSY path.

// BUSY-path transcript renderer — the original drawHUD body, now bottom-anchored
// under the sessions strip. Returns nothing; honors msgScroll / lineGen / the
// response-preview banner exactly as before.
static void drawHomeTranscript(const Palette& p, int topY) {
  const int SHOW = 2, LH = 16, WIDTH = 10;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.responseRcvdMs != 0 && (millis() - tama.responseRcvdMs) < 15000) {
    int by = H - AREA - 19;
    if (by < topY) by = topY;
    spr.fillRect(0, by, W, 18, DS_PANEL);
    spr.drawFastHLine(0, by, W, DS_AMBER);
    spr.setTextColor(DS_WHITE, DS_PANEL);
    spr.setTextSize(2);
    spr.setCursor(3, by + 1);
    int maxChars = (W - 6) / 12;
    int len = (int)strlen(tama.responsePreview);
    if (len <= maxChars) {
      spr.print(tama.responsePreview);
    } else {
      for (int i = 0; i < maxChars - 1; i++) spr.print(tama.responsePreview[i]);
      spr.print('>');
    }
    spr.setTextSize(1);
  }

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  static char disp[32][24];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }
  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;
  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  spr.setTextSize(2);
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? DS_WHITE : DS_DIM, p.bg);
    spr.setCursor(4, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextSize(2);
    spr.setTextColor(DS_ORANGE, p.bg);
    spr.setCursor(W - 36, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

// Orange DEMO badge — a small pill in the header's left third, drawn over the
// home screen whenever the fake-data demo mode is active (menu "demo" row).
// Sits just under the header strip so it never collides with the state cluster.
static void drawDemoBadge() {
  const char* lbl = "DEMO";
  const int bw = (int)strlen(lbl) * 6 + 8;   // size-1 caption + padding
  const int bx = 6, by = H_STRIP + 3, bh = 13;
  spr.fillRoundRect(bx, by, bw, bh, 2, DS_ORANGE);
  spr.setTextSize(FS_SUB);
  spr.setTextColor(DS_BLACK, DS_ORANGE);
  spr.setCursor(bx + 4, by + 3);
  spr.print(lbl);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();

  // ── HEADER STRIP ── busy when sessions are running or the persona is BUSY.
  bool busy = (tama.sessionsRunning > 0) || (activeState == P_BUSY);
  drawHeaderStrip(busy ? "BUSY" : "IDLE", busy ? DS_GREEN : DS_CYAN, batteryPct());
  // Demo-mode view = the redesigned Home plus an orange DEMO badge under the
  // header. Drawn here so it shows on both the IDLE and BUSY layouts.
  if (dataDemo()) drawDemoBadge();

  if (!busy) {
    // ScreenHomeIdle — "TOKENS TODAY" + hero number (period tokens mirror).
    const int STAT_TOP = 168;
    spr.fillRect(0, STAT_TOP, W, H - STAT_TOP, p.bg);
    spr.drawFastHLine(0, STAT_TOP, W, DS_LINE);
    spr.setTextSize(2);
    spr.setTextColor(DS_DIM, p.bg);
    spr.setCursor(6, STAT_TOP + 6);  spr.print("TOKENS");
    spr.setCursor(6, STAT_TOP + 24); spr.print("TODAY");
    char tb[12]; char unit = 0;
    fmtTokens(tb, sizeof(tb), g_dispTokens, &unit);
    spr.setTextSize(3);
    spr.setTextColor(DS_WHITE, p.bg);
    spr.setCursor(6, STAT_TOP + 44); spr.print(tb);
    if (unit) {
      int w3 = (int)strlen(tb) * 18;
      spr.setTextSize(2);
      spr.setTextColor(DS_DIM, p.bg);
      spr.setCursor(6 + w3 + 2, STAT_TOP + 51);
      spr.print(unit);
    }
    return;
  }

  // ScreenHomeWorking — RUN / WAIT / TOK inset strip, then transcript below it.
  const int STRIP_Y = 150, STRIP_H = 30;
  spr.fillRect(0, STRIP_Y, W, STRIP_H, p.bg);
  spr.drawFastHLine(0, STRIP_Y, W, DS_LINE);
  // three insets, widths 40 / 40 / 55 (TOK wider for the value).
  struct { const char* l; int x; int w; } cells[3] = {
    { "RUN", 3, 40 }, { "WAIT", 46, 40 }, { "TOK", 89, W - 89 - 3 }
  };
  char tokb[12]; char tunit = 0;
  fmtTokens(tokb, sizeof(tokb), g_dispTokens, &tunit);
  char tokv[14];
  if (tunit) snprintf(tokv, sizeof(tokv), "%s%c", tokb, tunit);
  else       snprintf(tokv, sizeof(tokv), "%s", tokb);
  for (int i = 0; i < 3; i++) {
    spr.fillRect(cells[i].x, STRIP_Y + 2, cells[i].w, STRIP_H - 4, DS_PANEL);
    spr.setTextSize(1);
    spr.setTextColor(DS_DIM, DS_PANEL);
    spr.setCursor(cells[i].x + 3, STRIP_Y + 4);
    spr.print(cells[i].l);
    spr.setTextSize(2);
    if (i == 0) {
      spr.setTextColor(DS_GREEN, DS_PANEL);
      spr.setCursor(cells[i].x + 3, STRIP_Y + 13);
      spr.printf("%u", tama.sessionsRunning);
    } else if (i == 1) {
      spr.setTextColor(DS_WHITE, DS_PANEL);
      spr.setCursor(cells[i].x + 3, STRIP_Y + 13);
      spr.printf("%u", tama.sessionsWaiting);
    } else {
      spr.setTextColor(DS_ORANGE, DS_PANEL);
      spr.setCursor(cells[i].x + 3, STRIP_Y + 13);
      spr.print(tokv);
    }
  }

  // The transcript rows are the only hud-gated part of Home: when the
  // transcript toggle is off, the RUN/WAIT/TOK session strip above still
  // renders (Home is never bare), but the per-line transcript is suppressed.
  if (settings().hud) {
    drawHomeTranscript(p, STRIP_Y + STRIP_H);
  }
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(0);
  M5.Imu.Init();
  M5.Beep.begin();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // off
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  // Single 2.4 GHz radio — WiFi and BLE are mutually exclusive. If both got
  // set, WiFi wins. In WiFi mode BLE is never initialized, so the radio is
  // fully free for WiFi + WireGuard (BLE coexistence was blocking the WG
  // handshake from completing).
  if (settings().wifi && settings().bt) { settings().bt = false; settingsSave(); }
  if (settings().bt) startBt();   // BLE only — WiFi is off in this branch
  petNameLoad();
  buddyInit();

  // Main sprite at 8-bit color depth (RGB332). Halves memory from
  // 65 KB → 32 KB, leaving enough heap for petSpr to allocate eagerly
  // even with WiFi/BLE running. Slight color quantization on the UI
  // is acceptable since most rendering uses a small accent palette.
  Serial.printf("[setup] heap before sprites=%u\n", (unsigned)ESP.getFreeHeap());
  spr.setColorDepth(8);
  spr.createSprite(W, H);
  // petSpr also 8-bit. Allocate eagerly right after main spr so it
  // gets first dibs on heap before WiFi/WebServer claim their share.
  petSpr.setColorDepth(8);
  petSprReady = (petSpr.createSprite(110, 135) != nullptr);
  Serial.printf("[setup] heap after sprites=%u, petSpr=%s\n",
                (unsigned)ESP.getFreeHeap(),
                petSprReady ? "ready" : "FAILED");

  micInit();
  micSetEnabled(settings().mic);   // honor saved opt-in state
  netWifiInit();                   // no-op unless WiFi mode (gated on s_wifi)
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0] && strcasecmp(ownerName(), petName()) != 0) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else if (ownerName()[0]) {
      // Owner name == pet name — just the name, no redundant "gr0m's gr0m".
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("gr0m appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

// Adapter mode — runtime-only (a reset clears it, returning to BT/WiFi pet
// mode). Strips the desk-pet rendering + mic so the device is a focused
// GPIO/logic probe; the command transports keep running.
bool adapterMode = false;
// DJ-booth scene flag — NOT a user toggle. Recomputed every frame in loop()
// (see djActive) purely from the character's state: it's the automatic end-game
// flourish, unlocked at Stage 5, played during celebrations and on a periodic
// idle drop-in. The scene itself is rendered in gr0m.cpp.
bool djActive = false;
// DJ-booth scene renderer — lives in gr0m.cpp (to reach its file-static
// draw primitives), composed onto the surface we pass in.
extern void gr0mRenderDJ(TFT_eSPI*, uint32_t);

// DJ scene tick — render the booth scene into the offscreen sprite and blit.
// Fully automatic: there is no exit button and no manual toggle. loop() drives
// it via djActive (Stage-5 celebrate, or the periodic idle flourish window),
// and stops calling it when that condition clears. Runs AFTER dataPoll +
// transport ticks so commands are still serviced each frame.
static void djTick(uint32_t now) {
  spr.fillSprite(TFT_BLACK);
  gr0mRenderDJ(&spr, now);
  spr.pushSprite(0, 0);
}

// Human-costume easter egg — a Stage-5 alternate "unconvincing human" look the
// user toggles by holding A+B together for 3s on the home screen. Rendered
// full-screen like the DJ scene; gr0m.cpp owns the drawing.
bool humanCostumeActive = false;
extern void gr0mRenderHumanCostume(TFT_eSPI*, uint32_t);
static void humanCostumeTick(uint32_t now) {
  spr.fillSprite(TFT_BLACK);
  gr0mRenderHumanCostume(&spr, now);
  spr.pushSprite(0, 0);
}

// ScreenAdapter — GPIO/logic-probe UI. Amber header ("ADAPTER" + PROBE dot),
// then one inset row per probe pin with G<n> · mode · live value · edge glyph,
// and a footer ("BtnB exit / 2Hz sample"). Live: digital pins are read with
// digitalRead, the ADC pin (GPIO36) with analogRead → volts. Composes into
// `spr`; adapterTick() handles the BtnB-exit, throttle, and blit.
static void drawAdapter() {
  // Probe set mirrors the M5StickC Plus header pins. 36 is ADC-only input.
  struct Pin { uint8_t n; const char* mode; bool adc; };
  static const Pin pins[] = {
    { 26, "IN ", false }, { 36, "ADC", true }, { 0, "IN ", false }, { 32, "IN ", false }
  };
  const int NP = sizeof(pins) / sizeof(pins[0]);

  spr.fillSprite(DS_BLACK);
  // Header — amber band, ADAPTER (amber) + PROBE dot.
  spr.fillRect(0, 0, W, 18, DS_ADPHDR);
  spr.setTextSize(1);
  spr.setTextColor(DS_AMBER, DS_ADPHDR);
  spr.setCursor(6, 5); spr.print("ADAPTER");
  drawStatusDot(W - 44, 8, DS_AMBER, 3, true);
  spr.setCursor(W - 36, 5); spr.print("PROBE");

  // Pin rows.
  int y = 24;
  const int RH = 22;
  for (int i = 0; i < NP; i++) {
    int ry = y + i * RH;
    spr.fillRect(4, ry, W - 8, RH - 3, DS_PANEL);
    spr.drawRect(4, ry, W - 8, RH - 3, DS_LINE);
    // G<n>
    spr.setTextSize(2);
    spr.setTextColor(DS_AMBER, DS_PANEL);
    spr.setCursor(8, ry + 2);
    spr.printf("G%u", pins[i].n);
    // mode (size 1, dim)
    spr.setTextSize(1);
    spr.setTextColor(DS_DIM, DS_PANEL);
    spr.setCursor(48, ry + 6);
    spr.print(pins[i].mode);
    // value + edge glyph
    char val[10]; uint16_t vcol; const char* edge;
    if (pins[i].adc) {
      int raw = analogRead(pins[i].n);              // 0..4095 over ~0..3.3V
      int mv = (raw * 3300) / 4095;
      snprintf(val, sizeof(val), "%d.%02dV", mv / 1000, (mv % 1000) / 10);
      vcol = DS_CYAN; edge = "~";
    } else {
      pinMode(pins[i].n, INPUT);
      bool hi = digitalRead(pins[i].n);
      snprintf(val, sizeof(val), "%s", hi ? "HI" : "LO");
      vcol = hi ? DS_GREEN : DS_DIM; edge = hi ? "^" : ".";
    }
    spr.setTextSize(2);
    spr.setTextColor(vcol, DS_PANEL);
    spr.setCursor(76, ry + 2);
    spr.print(val);
    spr.setTextSize(1);
    spr.setTextColor(DS_DIM, DS_PANEL);
    spr.setCursor(W - 14, ry + 6);
    spr.print(edge);
  }

  // Footer.
  int fy = H - 14;
  spr.fillRect(0, fy, W, 14, DS_DARK);
  spr.drawFastHLine(0, fy, W, DS_LINE);
  spr.setTextSize(1);
  spr.setTextColor(DS_DIM, DS_DARK);
  spr.setCursor(6, fy + 4); spr.print("BtnB exit");
  const char* fr = "2Hz sample";
  spr.setCursor(W - 6 - (int)strlen(fr) * 6, fy + 4); spr.print(fr);
}

// ScreenStageUp — the stage-up celebration overlay. The celebrating character
// is already composited into the middle of `spr`; this paints the top header
// band (EVOLUTION / STAGE n / stage name in pink "ASCENDED" style) and the
// bottom "DJ MODE!" payoff band, with a dark purple gradient tint top→bottom so
// the celebrate scene reads as a special moment. Drawn over the live frame.
static void drawStageUp(uint8_t stage) {
  if (stage > 5) stage = 5;
  // Top header band — gradient-ish dark purple.
  const int TH = 40;
  spr.fillRect(0, 0, W, TH, DS_SUTOP);
  spr.setTextSize(1);
  spr.setTextColor(DS_AMBER, DS_SUTOP);
  const char* el = "EVOLUTION";
  spr.setCursor((W - (int)strlen(el) * 6) / 2, 3); spr.print(el);
  spr.setTextSize(2);
  spr.setTextColor(DS_WHITE, DS_SUTOP);
  char sb[12]; snprintf(sb, sizeof(sb), "STAGE %u", stage);
  spr.setCursor((W - (int)strlen(sb) * 12) / 2, 14); spr.print(sb);
  spr.setTextSize(1);
  spr.setTextColor(DS_PINK, DS_SUTOP);
  const char* name = (stage >= 5) ? "ASCENDED" : EVO_STAGE_NAMES[stage];
  spr.setCursor((W - (int)strlen(name) * 6) / 2, 31); spr.print(name);
  spr.drawFastHLine(0, TH, W, DS_PINK);

  // Bottom payoff band.
  const int BH = 30, BY = H - BH;
  spr.fillRect(0, BY, W, BH, DS_SUBOT);
  spr.drawFastHLine(0, BY, W, DS_PINK);
  spr.setTextSize(2);
  spr.setTextColor(DS_PINK, DS_SUBOT);
  const char* dj = "DJ MODE!";
  spr.setCursor((W - (int)strlen(dj) * 12) / 2, BY + 4); spr.print(dj);
  spr.setTextSize(1);
  spr.setTextColor(DS_DIM, DS_SUBOT);
  const char* un = "unlocked";
  spr.setCursor((W - (int)strlen(un) * 6) / 2, BY + 21); spr.print(un);
}

static void adapterTick(uint32_t now) {
  // On-device exit: BtnB (the top button) leaves adapter mode without a
  // reset, resuming BLE advertising. A reset also returns to pet mode.
  if (M5.BtnB.wasPressed()) {
    adapterMode = false;
    bleAdvertisingStart();
    return;
  }
  static uint32_t last = 0;
  if (now - last < 500) return;              // light: ~2 Hz redraw
  last = now;
  drawAdapter();
  spr.pushSprite(0, 0);
}

void loop() {
  M5.update();
  M5.Beep.update();
  t++;
  uint32_t now = millis();

  // Read the serial/BLE command line FIRST, before any heavy work — the
  // buddy/character tick and the bottom-of-loop 3D render are the slow parts,
  // and an arriving permission prompt must not be stuck behind a slow frame.
  // dataPoll() parses incoming JSON (including the prompt + clearprompt cmds)
  // and the prompt-arrival block below draws the approval screen the same
  // iteration, so latency is bounded by parse time, not a full render frame.
  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = connOpen = usageOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();

      // Draw the approval screen immediately, this same iteration, instead of
      // waiting for the heavy 3D render at the bottom of the next loop. Compose
      // the buddy/character into the sprite (attention mood), overlay the
      // approval panel via drawHUD (which calls drawApproval while promptId is
      // set), and push it to the LCD now. The normal render path below still
      // runs every frame after this — this just removes the one-frame latency
      // between a prompt arriving and the user seeing it.
      if (!screenOff) {
        activeState = P_ATTENTION;
        if (buddyMode) {
          buddyTick(activeState);
        } else if (characterLoaded()) {
          characterSetState(activeState);
          characterTick();
        }
        drawHUD();
        spr.pushSprite(0, 0);
      }
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Pump the WiFi config portal (no-op when not in AP mode)
  netWifiTick();

  // WireGuard tunnel: bring it up once, the first time WiFi is ONLINE
  // AND the system clock is NTP-valid (WG handshake needs real time).
  // Then pump its status poll each loop. netWgInit() no-ops if there's
  // no wg_* config in NVS.
  static bool _wgStarted = false;
  if (!_wgStarted && netWifiOnline() && time(nullptr) > 1700000000) {
    _wgStarted = true;
    Serial.printf("[wg] starting tunnel, epoch=%ld\n", (long)time(nullptr));
    netWgInit();
  }
  if (_wgStarted) netWgTick();

  // TCP bridge listener — starts once WiFi is online (works over the LAN
  // and, once WG is up, over the tunnel). No-ops unless a token is set.
  static bool _tcpStarted = false;
  if (!_tcpStarted && netWifiOnline()) { _tcpStarted = true; netTcpInit(); }
  if (_tcpStarted) netTcpTick();

  // Adapter mode: device is a focused GPIO/logic probe — commands were just
  // serviced above (dataPoll + transports); skip the pet animation, mic, and
  // normal UI. A reset clears adapterMode and returns to BT/WiFi pet mode.
  if (adapterMode) { adapterTick(now); return; }

  // DJ scene: the automatic end-game flourish, unlocked only at Stage 5. No
  // manual toggle and no exit button — djActive is recomputed here every frame:
  //   1. Celebration: any time the character is celebrating (activeState ==
  //      P_CELEBRATE), play the DJ booth instead of the normal celebrate.
  //   2. Idle flourish: when otherwise idle, drop into the booth for ~8 s once
  //      every ~60 s (millis()-based window), then return to the normal pet.
  // Same placement as the adapter short-circuit — commands were just serviced
  // (dataPoll + transports above), so the link keeps working while it runs.
  {
    const uint32_t DJ_PERIOD_MS = 60000;   // idle cadence: a drop-in once a minute
    const uint32_t DJ_WINDOW_MS = 8000;    // and it lasts ~8 s
    bool stage5     = evoStage() >= 5;
    bool celebrate  = activeState == P_CELEBRATE;
    bool idle       = activeState == P_IDLE;
    bool flourish   = idle && (now % DJ_PERIOD_MS) < DJ_WINDOW_MS;
    djActive = stage5 && (celebrate || flourish);
  }
  if (djActive && !inPrompt) { djTick(now); delay(16); return; }   // a pending approval always wins the screen

  // A+B held together for 3s toggles the human-costume easter egg. Checked
  // every frame BEFORE the early-return below, so the same gesture also exits.
  {
    static uint32_t abHeldSince = 0;
    if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
      if (abHeldSince == 0) abHeldSince = now;
      else if (now - abHeldSince >= 3000) {
        humanCostumeActive = !humanCostumeActive;
        buddyInvalidate();
        beep(humanCostumeActive ? 2600 : 1400, 120);
        swallowBtnA = swallowBtnB = true;  // eat the release so it won't approve/deny/page
        abHeldSince = 0;
      }
    } else {
      abHeldSince = 0;
    }
  }
  if (humanCostumeActive && !inPrompt) { humanCostumeTick(now); delay(16); return; }   // approval wins over the easter egg too

  // Knock-to-approve: only acts during a permission prompt. 1 knock =
  // approve, 2+ knocks = deny. Outcome telemetry is owned by mic.cpp;
  // the indicator render reads it back via micLastAction*.
  micTick();
  if (inPrompt && micEnabled()) {
    uint8_t claps = micClapsConsume();
    if (claps == 1)      { sendApprove(); micRecordAction(1); }
    else if (claps >= 2) { sendDeny();    micRecordAction(2); }
  }

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (M5.Axp.GetBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      M5.Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else if (connOpen)  { connOpen = false; characterInvalidate(); }
    else if (usageOpen) { usageOpen = false; usageConfirmIdx = 0xFF; characterInvalidate(); }
    // On the CONNECTIONS / USAGE info pages, long-press opens that page's
    // edit submenu instead of the main menu (the page-advance B button
    // otherwise leaves no free gesture to enter them).
    else if (displayMode == DISP_INFO && infoPage == INFO_PG_CONNECTIONS) {
      connOpen = true; connSel = 0;
    }
    else if (displayMode == DISP_INFO && infoPage == INFO_PG_USAGE) {
      usageOpen = true; usageSel = 0; usageConfirmIdx = 0xFF;
    }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        sendApprove();
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (connOpen) {
        beep(1800, 30);
        connSel = (connSel + 1) % CONN_N;
      } else if (usageOpen) {
        beep(1800, 30);
        usageSel = (usageSel + 1) % USAGE_N;
        usageConfirmIdx = 0xFF;   // scrolling away disarms a pending confirm
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      sendDeny();
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (connOpen) {
      beep(2400, 30);
      applyConn(connSel);
    } else if (usageOpen) {
      beep(2400, 30);
      applyUsage(usageSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      // Entering the evolution showcase restarts the cycle at Stage 0 so it
      // always opens on the base form and walks up from there.
      if (petPage == PET_PG_SHOWCASE) showcaseStartMs = millis();
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  g_evoStage   = evoStage();      // refresh the mirrors gr0m.cpp renders from
  g_dispTokens = stats().tokens;

  // Evolution showcase override. The mirror above was just reset to the TRUE
  // evoStage(); here we transiently retarget it to the showcase's cycling
  // stage so buddyTick() (which runs below, reading g_evoStage) draws each
  // upgrade in turn. Because this is reapplied from truth every loop, simply
  // leaving the page — or opening a menu over it — restores the real stage on
  // the very next frame. No persistence, no statsSetTokens, nothing touched.
  bool showcasing = showcasePageActive() && !settingsOpen && !connOpen
                 && !usageOpen && !resetOpen && !inPrompt;
  if (showcasing) g_evoStage = showcaseStage();

  // Stage-5 DJ-unlock announce (one-time per boot). statsSetTokens latches
  // _djUnlocked the first time the character reaches Stage 5; when we observe
  // that edge live, play a brief celebration. This banner marks the moment the
  // automatic DJ flourish (see djActive in loop()) becomes available.
  static bool djWasUnlocked = statsDjUnlocked();
  static uint32_t djAnnounceUntil = 0;
  if (!djWasUnlocked && statsDjUnlocked()) {
    djWasUnlocked = true;
    djAnnounceUntil = now + 3000;
    triggerOneShot(P_CELEBRATE, 3000);
    beep(2400, 120);
  }
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !connOpen && !usageOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid()
               && (_onUsb || bleConnected() || netWifiOnline());   // show once linked, not USB-only
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    // The landscape clock draws direct-to-LCD and only full-clears on an
    // orientation flip (paintedOrient != clockOrient). Arriving at the clock
    // from another screen in the SAME orientation therefore leaves stale
    // pixels in the regions the clock layout doesn't overpaint (the gap
    // between the pet and the time). Force a full repaint on any clock
    // transition. 0xFF can never equal a real clockOrient (0/1/3).
    paintedOrient = 0xFF;
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff || landscapeClock) {
    // skip sprite render — face-down, powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(2);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    // Home always renders — it is never a bare character. drawHUD() draws the
    // design header strip + a stats footer regardless of the transcript
    // (settings().hud) toggle; hud only gates the BUSY transcript rows.
    else drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (connOpen) drawConn();
    else if (usageOpen) drawUsage();
    else if (menuOpen) drawMenu();
    drawWifiIndicator();
    drawWifiPortalBanner();
    drawListeningIndicator();
    // One-time Stage-5 stage-up celebration overlay (ScreenStageUp), shown for
    // the ~3s djAnnounceUntil window over the celebrating character.
    if ((int32_t)(now - djAnnounceUntil) < 0) {
      drawStageUp(evoStage());
    }
    // #tilt-worldup — when the setting is on and the device is held clearly
    // sideways, blit the sprite counter-rotated 90° so the head stays
    // visually vertical (the "rotate the screen orientation" approach the
    // SPEC calls out as the simplest acceptable impl). Skipped while any
    // overlay/menu is open (those carry upright text) and while charging in
    // a cradle. Falls back to the normal upright blit otherwise.
    bool worldUpRot = false;
    if (settings().worldUp && !inPrompt && !resetOpen && !settingsOpen && !connOpen
        && !usageOpen && !menuOpen && !blePasskey()) {
      float ax, ay, az;
      M5.Imu.getAccelData(&ax, &ay, &az);
      if (fabsf(ax) > 0.7f && fabsf(az) < 0.5f) {
        M5.Lcd.fillScreen(TFT_BLACK);   // clear the corners the rotated blit won't cover
        M5.Lcd.setPivot(W / 2, H / 2);
        spr.setPivot(W / 2, H / 2);
        // Counter-rotate against the device tilt so "up" stays up.
        spr.pushRotated((ax > 0) ? 90 : -90, TFT_BLACK);
        worldUpRot = true;
      }
    }
    if (!worldUpRot) spr.pushSprite(0, 0);
  }

  // #tilt-nap — face-down nap. SPEC: enter on gravity-down < -0.7g held 3s,
  // exit when |down| < 0.5g for 1s. (isFaceDown() reads the Z axis — the
  // physical "into the table" axis on this hardware is Z, which is what the
  // SPEC calls gravity_y in the sim's coordinate convention.) While napping
  // we render the SLEEP scene so the nightcap + z-particles show (drawn by
  // doSleep at Stage 4+), keep the screen dimmed, and refill energy at 5×.
  // Hold timers replace the old frame counter so the 3s / 1s windows are
  // wall-clock, not frame-rate, dependent. Skipped during approval.
  static uint32_t faceDownSinceMs = 0;   // 0 = not currently face-down
  static uint32_t faceUpSinceMs   = 0;   // 0 = not currently face-up (|az|<0.5)
  if (!inPrompt) {
    if (isFaceDown()) { if (!faceDownSinceMs) faceDownSinceMs = now; }
    else              faceDownSinceMs = 0;
    if (isFaceUp())   { if (!faceUpSinceMs) faceUpSinceMs = now; }
    else              faceUpSinceMs = 0;
  }

  if (!inPrompt && !napping && faceDownSinceMs && (now - faceDownSinceMs) >= 3000) {
    napping = true;
    napStartMs = now;
    statsNapBegin();              // capture energy baseline for the 5× refill
    M5.Axp.ScreenBreath(8);
    dimmed = true;
  } else if (!inPrompt && napping && faceUpSinceMs && (now - faceUpSinceMs) >= 1000) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // While napping: refill energy 5× and paint the sleep scene (nightcap +
  // z-particles) into the sprite so the face-down nap actually shows the
  // sleep mood per the SPEC, rather than freezing the prior frame.
  if (napping) {
    statsNapTick(now - napStartMs);
    if (!landscapeClock && buddyMode) {
      buddyTick(P_SLEEP);
      spr.pushSprite(0, 0);
    }
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Axp.SetLDO2(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
