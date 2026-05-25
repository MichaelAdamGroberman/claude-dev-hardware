#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Header-only with file-static state: include from exactly one translation
// unit (main.cpp). Including from a second .cpp produces duplicate symbols.

// Persistent stats backed by NVS. Load once at boot; save sparingly
// (NVS sectors have ~100K write cycles). We save on significant events
// only — approval, denial, nap end — never on a timer.

static const uint32_t TOKENS_PER_LEVEL = 50000;

struct Stats {
  uint32_t napSeconds;       // cumulative face-down time
  uint16_t approvals;
  uint16_t denials;
  uint16_t velocity[8];      // ring buffer: seconds-to-respond per approval
  uint8_t  velIdx;
  uint8_t  velCount;
  uint8_t  level;            // = lifetimeTokens / TOKENS_PER_LEVEL
  uint32_t tokens;           // PERIOD usage shown on the chest-LCD (daemon-driven, in-memory, resettable)
  uint32_t lifetimeTokens;   // monotonic lifetime usage — drives level + evoStage (persisted on stage-up)
  uint16_t okCount;          // approvals this period (daemon-driven display counter)
  uint16_t noCount;          // denials this period (daemon-driven display counter)
};

// Character evolution milestones (lifetime tokens): a new stage every ~5 levels
// (250K tokens), so the mascot visibly gains gear as you level up — Stage 1 by
// level 5, fully grown (and DJ unlocked) by level 25. Kept in sync with the
// daemon's _EVO_MILESTONES.
static const uint32_t EVO_MILESTONES[5] = {
  250000UL, 500000UL, 750000UL, 1000000UL, 1250000UL
};

static Stats _stats;
static Preferences _prefs;
static bool _dirty = false;

static bool _djUnlocked = false;   // sticky once the character reaches Stage 5

inline void statsLoad() {
  _prefs.begin("buddy", true);
  _stats.napSeconds = _prefs.getUInt("nap", 0);
  _stats.approvals  = _prefs.getUShort("appr", 0);
  _stats.denials    = _prefs.getUShort("deny", 0);
  _stats.velIdx     = _prefs.getUChar("vidx", 0);
  _stats.velCount   = _prefs.getUChar("vcnt", 0);
  // Lifetime tokens drive the level + evolution stage. The legacy "tok" key
  // held the same cumulative figure, so we keep reading it for continuity.
  _stats.lifetimeTokens = _prefs.getUInt("tok", 0);
  _stats.level      = _prefs.getUChar("lvl", 0);
  _djUnlocked       = _prefs.getBool("djunlk", false);
  size_t got = _prefs.getBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  if (got != sizeof(_stats.velocity)) memset(_stats.velocity, 0, sizeof(_stats.velocity));
  _prefs.end();
  // Backfill lifetime from a stored level if needed, then derive level from it.
  if (_stats.lifetimeTokens == 0 && _stats.level > 0) {
    _stats.lifetimeTokens = (uint32_t)_stats.level * TOKENS_PER_LEVEL;
  }
  _stats.level  = _stats.lifetimeTokens / TOKENS_PER_LEVEL;
  _stats.tokens = 0;   // period figure — daemon pushes the real value
  _stats.okCount = _stats.noCount = 0;
}

inline void statsSave() {
  if (!_dirty) return;
  _prefs.begin("buddy", false);
  _prefs.putUInt("nap", _stats.napSeconds);
  _prefs.putUShort("appr", _stats.approvals);
  _prefs.putUShort("deny", _stats.denials);
  _prefs.putUChar("vidx", _stats.velIdx);
  _prefs.putUChar("vcnt", _stats.velCount);
  _prefs.putUChar("lvl", _stats.level);
  _prefs.putUInt("tok", _stats.lifetimeTokens);   // lifetime drives level/evo
  _prefs.putBool("djunlk", _djUnlocked);
  _prefs.putBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  _prefs.end();
  _dirty = false;
}

// Level is token-driven now; approvals only feed mood/velocity.
inline void statsOnApproval(uint32_t secondsToRespond) {
  _stats.approvals++;
  _stats.velocity[_stats.velIdx] = (uint16_t)min(secondsToRespond, 65535u);
  _stats.velIdx = (_stats.velIdx + 1) % 8;
  if (_stats.velCount < 8) _stats.velCount++;
  _dirty = true; statsSave();
}

// Tokens feed the pet. 50K per level, 5K per pip on the fed bar.
// Bridge sends cumulative since its start; we add the delta. A drop means
// the bridge restarted — resync without adding, don't lose NVS progress.
static uint32_t _lastBridgeTokens = 0;
static bool _tokensSynced = false;       // first-sight latch — see below
static bool _levelUpPending = false;

inline void statsOnBridgeTokens(uint32_t bridgeTotal) {
  // The bridge sends its cumulative total since IT started. We track deltas.
  // Bridge restart → number drops → resync. But on DEVICE reboot,
  // _lastBridgeTokens is back to 0 while the bridge's total isn't — first
  // packet would re-credit the entire session. Latch on first sight instead.
  if (!_tokensSynced) {
    _lastBridgeTokens = bridgeTotal;
    _tokensSynced = true;
    return;
  }
  if (bridgeTotal < _lastBridgeTokens) {
    _lastBridgeTokens = bridgeTotal;     // bridge restarted
    return;
  }
  uint32_t delta = bridgeTotal - _lastBridgeTokens;
  _lastBridgeTokens = bridgeTotal;
  if (delta == 0) return;

  // Period display only. Level + lifetime are owned authoritatively by
  // statsSetTokens (the {"cmd":"tokens"} push carrying set/life), which also
  // fires the level-up celebration. Deriving level from the PERIOD counter
  // here set a wrong, period-based level and persisted a stale lifetimeTokens,
  // so this path no longer touches level/lifetime/NVS.
  _stats.tokens += delta;
}

inline bool statsPollLevelUp() {
  bool r = _levelUpPending;
  _levelUpPending = false;
  return r;
}

inline void statsOnDenial() { _stats.denials++; _dirty = true; statsSave(); }

inline void statsMarkDirty() { _dirty = true; }

inline void statsOnNapEnd(uint32_t seconds) {
  _stats.napSeconds += seconds;
  _dirty = true; statsSave();
}

// Median of the velocity ring buffer. 0 if empty.
inline uint16_t statsMedianVelocity() {
  if (_stats.velCount == 0) return 0;
  uint16_t tmp[8];
  memcpy(tmp, _stats.velocity, sizeof(tmp));
  uint8_t n = _stats.velCount;
  // insertion sort, n ≤ 8
  for (uint8_t i = 1; i < n; i++) {
    uint16_t k = tmp[i]; int8_t j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = k;
  }
  return tmp[n/2];
}

// 0..4 tier. Velocity sets the base; heavy denial ratio drags it down.
inline uint8_t statsMoodTier() {
  uint16_t vel = statsMedianVelocity();
  int8_t tier;
  if (vel == 0) tier = 2;              // no data: neutral
  else if (vel < 15) tier = 4;
  else if (vel < 30) tier = 3;
  else if (vel < 60) tier = 2;
  else if (vel < 120) tier = 1;
  else tier = 0;
  uint16_t a = _stats.approvals, d = _stats.denials;
  if (a + d >= 3) {                    // need a few decisions before judging
    if (d > a) tier -= 2;
    else if (d * 2 > a) tier -= 1;     // deny rate > 33%
  }
  if (tier < 0) tier = 0;
  return (uint8_t)tier;
}

// Energy: starts at 3/5 on boot, tops up to full on nap end, drains 1 tier per 2h.
static uint32_t _lastNapEndMs = 0;
static uint8_t  _energyAtNap  = 3;

inline void statsOnWake() { _lastNapEndMs = millis(); _energyAtNap = 5; }

inline uint8_t statsEnergyTier() {
  uint32_t hoursSince = (millis() - _lastNapEndMs) / 3600000;
  int8_t e = (int8_t)_energyAtNap - (int8_t)(hoursSince / 2);
  if (e < 0) e = 0; if (e > 5) e = 5;
  return (uint8_t)e;
}

inline uint8_t statsFedProgress() {
  return (uint8_t)((_stats.lifetimeTokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10));
}

// --- Settings --------------------------------------------------------------

struct Settings {
  bool sound;
  bool bt;
  bool wifi;     // placeholder — no WiFi stack linked yet, just stores the pref
  bool led;
  bool mic;      // clap-to-approve (mic sampling during permission prompts)
  bool hud;
  uint8_t clockRot;  // 0=auto 1=portrait 2=landscape
};

static Settings _settings = { true, true, false, true, false, true, 0 };

inline void settingsLoad() {
  _prefs.begin("buddy", true);
  _settings.sound = _prefs.getBool("s_snd", true);
  _settings.bt    = _prefs.getBool("s_bt",  true);
  _settings.wifi  = _prefs.getUChar("s_wifi", 0) != 0;
  _settings.led   = _prefs.getBool("s_led", true);
  _settings.mic   = _prefs.getBool("s_mic", false);
  _settings.hud      = _prefs.getBool("s_hud", true);
  _settings.clockRot = _prefs.getUChar("s_crot", 0);
  if (_settings.clockRot > 2) _settings.clockRot = 0;
  _prefs.end();
}

inline void settingsSave() {
  _prefs.begin("buddy", false);
  _prefs.putBool("s_snd", _settings.sound);
  _prefs.putBool("s_bt",  _settings.bt);
  _prefs.putBool("s_wifi",_settings.wifi);
  _prefs.putBool("s_led", _settings.led);
  _prefs.putBool("s_mic", _settings.mic);
  _prefs.putBool("s_hud", _settings.hud);
  _prefs.putUChar("s_crot", _settings.clockRot);
  _prefs.end();
}

static char _petName[24] = "gr0m";
static char _ownerName[32] = "";

inline void petNameLoad() {
  _prefs.begin("buddy", true);
  _prefs.getString("petname", _petName, sizeof(_petName));
  _prefs.getString("owner", _ownerName, sizeof(_ownerName));
  _prefs.end();
}

// Strip JSON-breaking chars — these names go into a printf'd JSON string
// unescaped (xfer.h status response). A quote persists to NVS and breaks
// the status endpoint until the name is re-set.
static void _safeCopy(char* dst, size_t dstLen, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
    char c = src[i];
    if (c != '"' && c != '\\' && c >= 0x20) dst[j++] = c;
  }
  dst[j] = 0;
}

inline void petNameSet(const char* name) {
  _safeCopy(_petName, sizeof(_petName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("petname", _petName);
  _prefs.end();
}

inline const char* petName() { return _petName; }

inline void ownerSet(const char* name) {
  _safeCopy(_ownerName, sizeof(_ownerName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("owner", _ownerName);
  _prefs.end();
}

inline const char* ownerName() { return _ownerName; }

inline uint8_t speciesIdxLoad() {
  _prefs.begin("buddy", true);
  uint8_t v = _prefs.getUChar("species", 0xFF);
  _prefs.end();
  return v;
}

inline void speciesIdxSave(uint8_t idx) {
  _prefs.begin("buddy", false);
  _prefs.putUChar("species", idx);
  _prefs.end();
}

inline Settings& settings() { return _settings; }

inline const Stats& stats() { return _stats; }

// Evolution stage 0..5 from lifetime usage crossing EVO_MILESTONES.
inline uint8_t evoStage() {
  uint8_t s = 0;
  for (uint8_t i = 0; i < 5; i++)
    if (_stats.lifetimeTokens >= EVO_MILESTONES[i]) s = i + 1;
  return s;
}

// Lifetime tokens at the NEXT milestone, or 0 if fully evolved (Stage 5).
inline uint32_t evoNextMilestone() {
  uint8_t s = evoStage();
  return (s < 5) ? EVO_MILESTONES[s] : 0;
}

inline bool statsDjUnlocked() { return _djUnlocked; }

// Push usage from the bridge daemon. In-memory, EXCEPT lifetime is persisted
// when the evolution stage advances (rare → negligible NVS wear) so a reboot
// lands on the right stage. period → chest-LCD; life → level + evoStage;
// ok/no → approvals/denials this period.
inline void statsSetTokens(uint32_t period, uint32_t life, uint16_t ok, uint16_t no) {
  _stats.tokens  = period;
  _stats.okCount = ok;
  _stats.noCount = no;
  uint8_t stageBefore = evoStage();
  uint8_t lvlBefore   = _stats.level;
  _stats.lifetimeTokens = life;
  _stats.level          = life / TOKENS_PER_LEVEL;
  if (_stats.level > lvlBefore) _levelUpPending = true;   // celebrate on level-up
  uint8_t stageAfter = evoStage();
  if (stageAfter != stageBefore) {
    if (stageAfter == 5 && !_djUnlocked) _djUnlocked = true;   // DJ bonus unlocked
    _dirty = true; statsSave();
  }
}

// Reset the PERIOD counters (token usage + approved/denied) shown on-device.
// The daemon holds the authoritative baseline; this zeroes the local display
// immediately for snappy feedback.
inline void statsResetCounters() {
  _stats.tokens = 0;
  _stats.okCount = _stats.noCount = 0;
}

// Reset the buddy LEVEL — wipe lifetime progress so the character de-evolves to
// Stage 0. Persisted immediately. The daemon's lifetime baseline is reset in
// parallel (level_reset event) so it won't re-push the old total.
inline void statsResetLevel() {
  _stats.lifetimeTokens = 0;
  _stats.level = 0;
  _dirty = true; statsSave();
}
