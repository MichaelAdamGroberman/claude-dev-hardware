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

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

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
const uint8_t PET_PAGES = 2;
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

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
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
const char* settingsItems[] = { "brightness", "sound", "led", "mic claps", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 9;
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
    case 6: nextPet(); return;       // ascii pet
    case 7: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 8: settingsOpen = false; characterInvalidate(); return;
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

// v2 design: full-width card, orange header pill, 4-px orange left bar
// marks the selected row, dim divider above the footer hint strip.
// Settings stays at size 1 because "brightness"/"transcript" are 10
// chars wide and at size 2 (12 px/char) the label overflows the value
// column. Menu and Reset move to size 2 — those lists are shorter.

static void drawSettings() {
  const Palette& p = characterPalette();
  const int mw = W - 8, mx = 4;
  const int HEADER_H = 18, FOOTER_H = 16, ROW_H = 14;
  const int mh = HEADER_H + SETTINGS_N * ROW_H + FOOTER_H;
  const int my = (H - mh) / 2;

  // Card chrome
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);

  // Header pill
  spr.fillRoundRect(mx, my, mw, HEADER_H, 4, p.body);
  spr.setTextSize(1);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(mx + 6, my + 6);
  spr.print("SETTINGS");

  Settings& s = settings();
  // On/off rows in the new layout: 1=sound 2=led 3=mic 4=transcript.
  // 5=clock rot (enum), 6=ascii pet (count); 0=brightness; 7/8 = reset/back
  // (no value column).
  int rowsTop = my + HEADER_H + 2;
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    int ry = rowsTop + i * ROW_H;
    if (sel) {
      spr.fillRect(mx + 1, ry - 1, 3, ROW_H, p.body);   // orange left bar
    }
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 8, ry + 3);
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 38, ry + 3);
    if (i == 0) {
      spr.setTextColor(p.body, PANEL);
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 4) {
      bool on = (i == 1) ? s.sound : (i == 2) ? s.led : (i == 3) ? s.mic : s.hud;
      spr.setTextColor(on ? GREEN : p.textDim, PANEL);
      spr.print(on ? " on" : "off");
    } else if (i == 5) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.setTextColor(p.body, PANEL);
      spr.print(RN[s.clockRot]);
    } else if (i == 6) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.setTextColor(p.body, PANEL);
      spr.printf("%u/%u", pos, total);
    }
  }

  // Footer hint
  int fy = my + mh - FOOTER_H;
  spr.drawFastHLine(mx + 4, fy, mw - 8, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 6, fy + 4);
  spr.print("A Next   B Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  const int mw = W - 8, mx = 4;
  const int HEADER_H = 22, FOOTER_H = 16, ROW_H = 22;
  const int mh = HEADER_H + RESET_N * ROW_H + FOOTER_H;
  const int my = (H - mh) / 2;

  // Card chrome — red border to signal danger
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);

  // Header pill — RED for reset
  spr.fillRoundRect(mx, my, mw, HEADER_H, 4, HOT);
  spr.setTextSize(2);
  spr.setTextColor(0x0000, HOT);
  spr.setCursor(mx + 6, my + 4);
  spr.print("RESET");

  int rowsTop = my + HEADER_H + 2;
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    int ry = rowsTop + i * ROW_H;
    if (sel) spr.fillRect(mx + 1, ry, 3, ROW_H - 2, p.body);
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    spr.setTextSize(2);
    spr.setTextColor(armed ? HOT : (sel ? p.text : p.textDim), PANEL);
    spr.setCursor(mx + 8, ry + 4);
    spr.print(armed ? "really?" : resetItems[i]);
  }

  // Footer
  int fy = my + mh - FOOTER_H;
  spr.drawFastHLine(mx + 4, fy, mw - 8, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 6, fy + 4);
  spr.print("A Next   B Confirm");
}

// Connection submenu — radio (WiFi/BT/Off) + Adapter. Styled like
// drawSettings (size-1 rows) so the labels and the on/off-ish state column
// fit the 135-px width. Opened from the CONNECTIONS info page.
static void drawConn() {
  const Palette& p = characterPalette();
  const int mw = W - 8, mx = 4;
  const int HEADER_H = 18, FOOTER_H = 16, ROW_H = 18;
  const int mh = HEADER_H + CONN_N * ROW_H + FOOTER_H;
  const int my = (H - mh) / 2;

  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);

  spr.fillRoundRect(mx, my, mw, HEADER_H, 4, p.body);
  spr.setTextSize(1);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(mx + 6, my + 6);
  spr.print("CONNECTION");

  Settings& s = settings();
  // Which radio mode is active now, so the row shows a live indicator.
  bool wifiOn = s.wifi, btOn = s.bt, off = !s.wifi && !s.bt;
  int rowsTop = my + HEADER_H + 3;
  for (int i = 0; i < CONN_N; i++) {
    bool sel = (i == connSel);
    int ry = rowsTop + i * ROW_H;
    if (sel) spr.fillRect(mx + 1, ry - 1, 3, ROW_H, p.body);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 8, ry + 4);
    spr.print(connItems[i]);
    // Active-radio dot on the matching row.
    bool active = (i == 0 && wifiOn) || (i == 1 && btOn) || (i == 2 && off);
    if (active) {
      spr.setTextColor(GREEN, PANEL);
      spr.setCursor(mx + mw - 16, ry + 4);
      spr.print("o");
    }
  }

  int fy = my + mh - FOOTER_H;
  spr.drawFastHLine(mx + 4, fy, mw - 8, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 6, fy + 4);
  spr.print("A Next   B Select");
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

// Usage submenu — reporting span + period/level resets. Styled like
// drawSettings. Opened from the USAGE info page.
static void drawUsage() {
  const Palette& p = characterPalette();
  const int mw = W - 8, mx = 4;
  const int HEADER_H = 18, FOOTER_H = 16, ROW_H = 20;
  const int mh = HEADER_H + USAGE_N * ROW_H + FOOTER_H;
  const int my = (H - mh) / 2;

  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);

  spr.fillRoundRect(mx, my, mw, HEADER_H, 4, p.body);
  spr.setTextSize(1);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(mx + 6, my + 6);
  spr.print("USAGE");

  int rowsTop = my + HEADER_H + 3;
  for (int i = 0; i < USAGE_N; i++) {
    bool sel = (i == usageSel);
    int ry = rowsTop + i * ROW_H;
    if (sel) spr.fillRect(mx + 1, ry - 1, 3, ROW_H, p.body);
    bool armed = (i == usageConfirmIdx) &&
                 (int32_t)(millis() - usageConfirmUntil) < 0;
    spr.setTextColor(armed ? HOT : (sel ? p.text : p.textDim), PANEL);
    spr.setCursor(mx + 8, ry + 5);
    if (i == 0) {
      // span row carries its current label inline.
      spr.printf("span: %s", USAGE_SPANS[usageSpanIdx]);
    } else if (armed) {
      // reset level is the destructive one — make the confirm read "really?".
      spr.print(i == 2 ? "really?!" : "really?");
    } else {
      spr.print(usageItems[i]);
    }
  }

  int fy = my + mh - FOOTER_H;
  spr.drawFastHLine(mx + 4, fy, mw - 8, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 6, fy + 4);
  spr.print("A Next   B Change");
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

// Main menu — matched to drawSettings/drawConn/drawUsage so all the menus
// share one look: 18-px header pill (size-1 label), 18-px size-1 rows, the
// same 3-px orange left bar marking the selection, and the same footer strip.
void drawMenu() {
  const Palette& p = characterPalette();
  const int mw = W - 8, mx = 4;
  const int HEADER_H = 18, FOOTER_H = 16, ROW_H = 18;
  const int mh = HEADER_H + MENU_N * ROW_H + FOOTER_H;
  const int my = (H - mh) / 2;

  // Card chrome
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);

  // Header pill — orange
  spr.fillRoundRect(mx, my, mw, HEADER_H, 4, p.body);
  spr.setTextSize(1);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(mx + 6, my + 6);
  spr.print("MENU");

  int rowsTop = my + HEADER_H + 3;
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    int ry = rowsTop + i * ROW_H;
    if (sel) spr.fillRect(mx + 1, ry - 1, 3, ROW_H, p.body);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 8, ry + 4);
    spr.print(menuItems[i]);
    if (i == 4) {
      bool on = dataDemo();
      spr.setTextColor(on ? GREEN : p.textDim, PANEL);
      spr.setCursor(mx + mw - 28, ry + 4);
      spr.print(on ? "on" : "off");
    }
  }

  // Footer
  int fy = my + mh - FOOTER_H;
  spr.drawFastHLine(mx + 4, fy, mw - 8, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 6, fy + 4);
  spr.print("A Next   B Select");
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




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  // Smaller orange pill — label dropped to size 1 and pill height
  // shrunk 22 → 14 px so it's less visually dominant on every Info page.
  const int HEADER_H = 14;
  spr.fillRoundRect(4, y, W - 8, HEADER_H, 3, p.body);
  spr.setTextSize(1);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(8, y + 4);
  spr.print(section);
  // Page counter right-aligned
  char pb[8]; snprintf(pb, sizeof(pb), "%u/%u", page + 1, INFO_PAGES);
  int plen = strlen(pb);
  spr.setCursor(W - 8 - plen * 6, y + 4);
  spr.print(pb);
  y += HEADER_H + 6;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  const uint16_t BORDER = 0x05FF;   // cyan; named locally to avoid the
                                    // CYAN macro from TFT_eSPI/In_eSPI.h
  spr.fillSprite(p.bg);

  // Header pill — orange (consistent with menu/settings)
  spr.fillRoundRect(4, 40, W - 8, 22, 4, p.body);
  spr.setTextSize(2);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(10, 44);
  spr.print("PAIRING");

  // Cyan border around the digit group
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  const int digW = 18 * 6;            // 6 digits × 18 px at size 3
  const int dx   = (W - digW) / 2 - 6;
  const int dy   = 100;
  const int dw   = digW + 12;
  const int dh   = 30;
  spr.drawRoundRect(dx,     dy,     dw,     dh,     5, BORDER);
  spr.drawRoundRect(dx - 1, dy - 1, dw + 2, dh + 2, 6, BORDER);

  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor((W - digW) / 2, dy + 3);
  spr.print(b);

  // Sub-line
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 180); spr.print("enter on desktop");
  spr.setCursor(8, 192); spr.print("> Developer");
  spr.setCursor(8, 204); spr.print("> Hardware Buddy");
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
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    lg("I watch your");
    lg("Claude desktop.");
    y += 4;
    lg("Sleep when idle,");
    lg("wake when busy,");
    lg("fret on prompts.");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    lg("Press A to");
    lg("approve.");

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
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

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
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
    // All three transports on one page. State is read from cached getters
    // (netWifiState / netWgState are updated in the tick loop, and RSSI is
    // cached too) — nothing here polls a driver per render frame.
    _infoHeader(p, y, "CONNECTIONS", infoPage);

    // ── Bluetooth ──
    bool linked = settings().bt && dataBtActive();
    spr.setTextColor(p.text, p.bg); ln("BT");
    spr.setTextColor(linked ? GREEN : (settings().bt ? 0xFFE0 : p.textDim), p.bg);
    ln("  %s", linked ? "linked" : (settings().bt ? "discoverable" : "off"));

    // ── WiFi ──
    y += 4;
    NetWifiState ws = netWifiState();
    spr.setTextColor(p.text, p.bg); ln("WIFI");
    uint16_t wc = (ws == NW_ONLINE) ? GREEN : (ws == NW_FAILED) ? HOT
                : (ws == NW_OFF) ? p.textDim : 0xFFE0;
    const char* wl = (ws == NW_ONLINE) ? "online" : (ws == NW_OFF) ? "off"
                   : (ws == NW_PORTAL) ? "setup AP" : (ws == NW_FAILED) ? "failed"
                   : "connecting";
    spr.setTextColor(wc, p.bg); ln("  %s", wl);
    if (ws == NW_ONLINE) {
      spr.setTextColor(p.textDim, p.bg);
      ln("  %s", netWifiIP());
    }

    // ── VPN (WireGuard) — only once a tunnel is configured ──
    if (netWgState() != WG_OFF) {
      y += 4;
      NetWgState gs = netWgState();
      spr.setTextColor(p.text, p.bg); ln("VPN");
      spr.setTextColor(gs == WG_UP ? GREEN : gs == WG_FAILED ? HOT : 0xFFE0, p.bg);
      ln("  %s", gs == WG_UP ? "up" : gs == WG_FAILED ? "failed" : "connecting");
      if (gs == WG_UP) { spr.setTextColor(p.textDim, p.bg); ln("  %s", netWgTunnelIP()); }
    }

    // Hint: BtnA long-press opens the Connection submenu (gated in loop()).
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12); spr.print("hold A: edit");

  } else if (infoPage == INFO_PG_USAGE) {
    _infoHeader(p, y, "USAGE", infoPage);

    // Period token figure (chest-LCD source) — hero number.
    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%lu", (unsigned long)stats().tokens);   // full exact count — the Usage screen is the detailed view
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, y + 18); spr.print("tokens");
    y += 32;

    // Approved / denied this period.
    spr.setTextColor(0x07E0, p.bg); spr.setCursor(4, y);  spr.printf("OK %u", stats().okCount);
    spr.setTextColor(HOT,    p.bg); spr.setCursor(68, y); spr.printf("NO %u", stats().noCount);
    y += 14;

    // Level (lifetime-derived monotonic score).
    spr.setTextColor(p.text, p.bg); spr.setCursor(4, y);
    spr.printf("LV %u", stats().level);
    y += 14;

    // Evolution stage + next milestone (e.g. "Stage 3/5 ->100M").
    uint32_t nxt = evoNextMilestone();
    char mb[8];
    if (nxt == 0)              snprintf(mb, sizeof(mb), "max");
    else if (nxt >= 1000000UL) snprintf(mb, sizeof(mb), "%luM", (unsigned long)(nxt / 1000000UL));
    else                       snprintf(mb, sizeof(mb), "%luK", (unsigned long)(nxt / 1000UL));
    spr.setTextColor(p.body, p.bg); spr.setCursor(4, y);
    spr.printf("Stage %u/5 ->%s", evoStage(), mb);
    y += 14;

    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12); spr.print("hold A: edit");

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
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.fillRect(0, TOP, W, 22, ALARM);
  spr.setTextSize(2);
  spr.setTextColor(0x0000, ALARM);
  spr.setCursor(6, TOP + 4);
  spr.print("APPROVE");
  // Source badge — bridge tags prompts with "cli" / "app" / "mob".
  // Sits to the right of APPROVE in size 1 so you can tell at a glance
  // whether the prompt came from Claude Code, the desktop app, etc.
  if (tama.promptSrc[0]) {
    spr.setTextSize(1);
    spr.setTextColor(0xFFFF, ALARM);
    spr.setCursor(6 + 7 * 12 + 4, TOP + 8);
    spr.print(tama.promptSrc);
  }
  // elapsed time, right-aligned, turns yellow after 10s
  spr.setTextSize(2);
  char tb[8]; snprintf(tb, sizeof(tb), "%lus", (unsigned long)waited);
  int tlen = strlen(tb);
  spr.setTextColor(waited >= 10 ? 0xFFE0 : 0x0000, ALARM);
  spr.setCursor(W - tlen * 12 - 6, TOP + 4);
  spr.print(tb);

  // ── TOOL NAME (huge) ─────────────────────────────────────────────
  spr.setTextSize(1);
  spr.setTextColor(HOT, p.bg);
  spr.setCursor(6, TOP + 28);
  spr.print("TOOL");

  int toolLen = strlen(tama.promptTool);
  // Tool-name title one notch smaller than before (was up to size 3): size 2
  // (16px glyph, ~8 chars in 135 wide), falling back to size 1 when long.
  uint8_t toolSize = (toolLen <= 9) ? 2 : 1;
  spr.setTextSize(toolSize);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, TOP + 38);
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // ── COMMAND PREVIEW ──────────────────────────────────────────────
  int previewY = TOP + 38 + (toolSize == 2 ? 18 : 12);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, previewY);
  spr.print("RUN");
  spr.setTextColor(p.text, p.bg);
  int hlen = strlen(tama.promptHint);
  // Command preview in Font 2 (8x16) — a step up from the size-1 default so
  // it's easier to read. ~18 proportional chars/line, two lines above the
  // footer (the footer is drawn afterward and covers any 1-2px overrun).
  spr.setTextFont(2);
  spr.setTextSize(1);
  spr.setCursor(6, previewY + 10);
  spr.printf("%.18s", tama.promptHint);
  if (hlen > 18) {
    spr.setCursor(6, previewY + 26);
    spr.printf("%.18s", tama.promptHint + 18);
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
  int y = 88;
  spr.fillRoundRect(6, y, 46, 18, 3, p.body);
  spr.setTextSize(1);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 5);
  spr.print("LV");
  spr.setTextSize(2);
  spr.setCursor(26, y + 2);
  spr.printf("%u", stats().level);
  spr.setTextSize(1);

  // ── FED METER ───────────────────────────────────────────────────
  y = 112;
  spr.setTextColor(p.textDim, p.bg);
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
  uint16_t batCol = (pct >= 50) ? 0x07E0 : (pct >= 20) ? 0xFFE0 : HOT;

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.print("BATTERY");
  // Mode badge on the right of the label row — right-aligned with 4 px margin.
  const char* badge = charging ? "CHG" : (usb ? "USB" : nullptr);
  if (badge) {
    spr.setTextColor(charging ? 0xFFE0 : 0x05FF, p.bg);
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
  auto cell = [&](int cx, int cy, const char* label, uint16_t labelCol, uint16_t valCol, const char* fmt, uint32_t v) {
    spr.setTextSize(1);
    spr.setTextColor(labelCol, p.bg);
    spr.setCursor(cx, cy);
    spr.print(label);
    spr.setTextSize(2);
    spr.setTextColor(valCol, p.bg);
    spr.setCursor(cx, cy + 10);
    spr.printf(fmt, (unsigned long)v);
    spr.setTextSize(1);
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
  cell(6,    y, "APPROVED", p.textDim, 0x07E0, "%lu", stats().approvals);
  cell(COL2, y, "DENIED",   p.textDim, HOT,    "%lu", stats().denials);

  // Row 2 — TOKENS, NAPPED
  y += 32;
  char tbuf[12];
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.print("TOKENS");
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(2);
  spr.setCursor(6, y + 10);
  spr.print(tokStr(tbuf, sizeof(tbuf), stats().tokens));
  spr.setTextSize(1);

  uint32_t nap = stats().napSeconds;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(COL2, y);
  spr.print("NAPPED");
  spr.setTextSize(2);
  spr.setTextColor(0x05FF, p.bg);
  spr.setCursor(COL2, y + 10);
  spr.printf("%luh%02lu", nap / 3600, (nap / 60) % 60);
  spr.setTextSize(1);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  int y = TOP + 14;            // room for the PET header drawn by drawPet()
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

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title only (no page counter).
  // Dynamic size: size 2 (12 px/char) fits ~10 chars at x=4 on the 135 px
  // screen. Longer owner names (e.g. "Claude Code's gr0m" = 17 chars)
  // would wrap to a second line at size 2, so fall back to size 1
  // (6 px/char, ~21 chars fit) before that happens.
  char title[40];
  const char* owner = ownerName();
  // If the owner name matches the pet's name (e.g. owner "gr0m" + the gr0m
  // species), skip the redundant "gr0m's gr0m" and just show the name.
  if (owner[0] && strcasecmp(owner, petName()) != 0) {
    snprintf(title, sizeof(title), "%s's %s", owner, petName());
  } else {
    snprintf(title, sizeof(title), "%s", petName());
  }
  spr.setTextSize(strlen(title) <= 10 ? 2 : 1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  spr.print(title);
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

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  // Transcript at setTextSize(2): glyphs are 12×16 px so WIDTH = 10
  // chars/line (10·12 = 120 px ≤ 135) and SHOW = 2 rows.
  const int SHOW = 2, LH = 16, WIDTH = 10;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);   // banner below uses size-1 metrics

  // Response preview banner — shows the most recent `evt: turn` text from
  // the desktop for ~15s after it arrives, then fades. Single-line truncated.
  if (tama.responseRcvdMs != 0 && (millis() - tama.responseRcvdMs) < 15000) {
    int by = H - AREA - 19;            // banner sits just above the entries
    spr.fillRect(0, by, W, 18, 0x18C3); // dark slate background, taller for size-2 text
    spr.drawFastHLine(0, by, W, 0xFFE0);
    spr.setTextColor(0xFFFF, 0x18C3);
    spr.setTextSize(2);
    spr.setCursor(3, by + 1);
    // Manual truncate with ">" sentinel so long previews fit the width
    int maxChars = (W - 6) / 12;       // 12 px per char at size 2
    int len = (int)strlen(tama.responsePreview);
    if (len <= maxChars) {
      spr.print(tama.responsePreview);
    } else {
      for (int i = 0; i < maxChars - 1; i++) spr.print(tama.responsePreview[i]);
      spr.print('>');                  // truncation marker
    }
    spr.setTextSize(1);                // restore default for whatever follows
  }

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
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
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(4, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextSize(2);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 36, H - LH - 2);  // 3 chars × 12 px at size 2 = 36 px
    spr.printf("-%u", msgScroll);
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
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
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
  spr.fillSprite(TFT_BLACK);
  spr.setTextColor(TFT_GREEN, TFT_BLACK);
  spr.setTextSize(2); spr.setCursor(8, 24); spr.print("ADAPTER");
  spr.setTextSize(1); spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setCursor(8, 54); spr.print("GPIO / logic probe");
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.setCursor(8, 74); spr.print("pins 0 25 26");
  spr.setCursor(8, 86); spr.print("     32 33 36");
  spr.setTextColor(0x07FF, TFT_BLACK);
  spr.setCursor(8, 110); spr.print("BtnB or reset: exit");
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
  if (djActive) { djTick(now); return; }

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
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (connOpen) drawConn();
    else if (usageOpen) drawUsage();
    else if (menuOpen) drawMenu();
    drawWifiIndicator();
    drawWifiPortalBanner();
    drawListeningIndicator();
    // One-time Stage-5 "DJ MODE!" payoff banner (see djAnnounceUntil above).
    if ((int32_t)(now - djAnnounceUntil) < 0) {
      const Palette& p = characterPalette();
      spr.fillRoundRect(8, 96, W - 16, 30, 5, p.body);
      spr.drawRoundRect(8, 96, W - 16, 30, 5, 0xFFFF);
      spr.setTextSize(2);
      spr.setTextColor(p.bg, p.body);
      spr.setCursor((W - 8 * 12) / 2, 104);
      spr.print("DJ MODE!");
    }
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Axp.ScreenBreath(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
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
