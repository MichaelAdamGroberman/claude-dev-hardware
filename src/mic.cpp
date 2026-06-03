#include "mic.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <stdlib.h>
#include <math.h>

// ── M5StickC Plus SPM1423 PDM mic pinout ─────────────────────────────
static const int         PDM_CLK_PIN = 0;
static const int         PDM_DAT_PIN = 34;
static const int         SAMPLE_RATE = 16000;
static const i2s_port_t  I2S_PORT    = I2S_NUM_0;

// ── Knock / clap detection tuning ───────────────────────────────────
// The detector requires BOTH conditions to fire:
//   (a) absolute peak > ABSOLUTE_FLOOR — guards against silent rooms
//       where the adaptive baseline would be near zero
//   (b) peak > _baseline * ONSET_RATIO — guards against sustained
//       noise (HVAC, music, talking) that would slowly raise the
//       baseline. Only sharp transients (sudden jumps) trigger.
// This adaptive approach self-tunes to the user's environment.
static const int      ABSOLUTE_FLOOR = 1500;
static const float    ONSET_RATIO    = 3.0f;
static const float    BASELINE_ALPHA = 0.05f;   // baseline IIR factor
static const uint32_t REFRACTORY_MS  = 150;
static const uint32_t WINDOW_MS      = 600;

static bool     _enabled     = false;
static bool     _initialized = false;
static uint32_t _lastClapMs  = 0;
static uint32_t _windowEndMs = 0;
static uint8_t  _clapCount   = 0;
static int      _lastPeak    = 0;       // most recent frame peak (for UI meter)
static float    _baseline    = 1500.f;  // rolling baseline of recent peaks

// ── Live audio-visualization analysis (read-only animation feed) ─────
// This is a SEPARATE, side-effect-free path from the clap detector above.
// It never writes _clapCount / _windowEndMs / _lastClapMs — it only reads
// the same DMA frame to derive a spectrum, a loudness envelope, and a
// beat-onset pulse for UI scenes. It is updated only while sampling is
// enabled; when disabled everything below decays/reads as 0.
//
// 7 log-spaced Goertzel center frequencies for the filter bank, chosen to
// cover the audible band sampled at 16 kHz (Nyquist 8 kHz):
//   ~125 / 250 / 500 / 1000 / 2000 / 4000 / 6000 Hz, low→high.
static const float    VIZ_FREQ[7] = {
  125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 6000.f
};
// Per-band auto-gain running max (slow) so a loud band normalizes to ~full
// scale and a quiet room doesn't peg the bars. Decays so the meter re-arms.
static float    _vizMax[7]   = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
// Smoothed displayed band values (0..10 float) — rise fast, fall slow.
static float    _vizBand[7]  = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
// Published integer band magnitudes 0..10 (what micSpectrum() returns).
static uint8_t  _vizSpec[7]  = { 0, 0, 0, 0, 0, 0, 0 };
// Smoothed overall loudness 0..255 (envelope of peak above baseline).
static float    _vizLevel    = 0.f;
// Moving average of the loudness used as the beat-onset reference.
static float    _vizLevelAvg = 0.f;
// One-shot beat flag (set on onset, cleared by micBeat()) + refractory.
static bool     _vizBeat     = false;
static uint32_t _vizBeatMs   = 0;

// Auto-gain / smoothing tuning for the visualization feed.
static const float    VIZ_MAX_DECAY   = 0.995f;  // per-frame running-max bleed
static const float    VIZ_RISE        = 0.60f;   // bar attack (fast)
static const float    VIZ_FALL        = 0.12f;   // bar release (slow)
static const float    VIZ_LVL_RISE    = 0.50f;   // loudness attack
static const float    VIZ_LVL_FALL    = 0.08f;   // loudness release
static const float    VIZ_AVG_ALPHA   = 0.10f;   // beat-reference IIR
static const float    VIZ_BEAT_RATIO  = 1.28f;   // onset = level > avg*ratio (lower = more beats: kicks + snares)
static const float    VIZ_BEAT_FLOOR  = 16.f;    // min level to count a beat (lower = catches moderate-volume music)
static const uint32_t VIZ_BEAT_REFRACT_MS = 110; // debounce between beats (lower = allows faster beats)

// Sustained-audio ("music") detector. A clap is a brief transient out of
// quiet; music holds the level up. When loudness stays above MUSIC_LEVEL_THRESH
// for > MUSIC_SUSTAIN_MS we enter music mode, which SUPPRESSES clap
// registration (so room music can't approve/deny live prompts). It releases
// MUSIC_RELEASE_MS after the audio drops back to quiet, so deliberate isolated
// claps still work.
static const int      MUSIC_LEVEL_THRESH = 35;   // _vizLevel above this = "loud"
static const uint32_t MUSIC_SUSTAIN_MS   = 400;  // loud this long => music
static const uint32_t MUSIC_RELEASE_MS   = 250;  // quiet this long => music ends
static uint32_t _loudSinceMs  = 0;
static uint32_t _quietSinceMs = 0;
static bool     _musicMode    = false;

// Reset the visualization feed to its quiet/zero state (used when mic
// sampling is turned off so callers cheaply see "no signal").
static void vizReset() {
  for (int i = 0; i < 7; i++) { _vizBand[i] = 0.f; _vizSpec[i] = 0; _vizMax[i] = 1.f; }
  _vizLevel = 0.f; _vizLevelAvg = 0.f; _vizBeat = false; _vizBeatMs = 0;
  _loudSinceMs = 0; _quietSinceMs = 0; _musicMode = false;
}

// Run the per-frame spectral + loudness + onset analysis. Pure read of the
// frame `buf` (n samples); updates only the _viz* state above. `peak` is the
// frame peak already computed by micTick(); `baseline` is the adaptive floor.
static void vizAnalyze(const int16_t* buf, int n, int peak, float baseline) {
  if (n <= 0) return;

  // ── 7-band Goertzel filter bank over the frame ────────────────────
  // Goertzel: for k = round(N * f / fs), recurrence s[m] = x[m] + coeff*s[m-1]
  // - s[m-2], coeff = 2*cos(2*pi*k/N); magnitude^2 = s1^2 + s2^2 - coeff*s1*s2.
  // N samples × 7 bands of one multiply-add each — trivially cheap.
  float coeff[7];
  for (int b = 0; b < 7; b++) {
    float k = roundf((float)n * VIZ_FREQ[b] / (float)SAMPLE_RATE);
    coeff[b] = 2.0f * cosf(2.0f * (float)M_PI * k / (float)n);
  }
  float s1[7] = {0,0,0,0,0,0,0}, s2[7] = {0,0,0,0,0,0,0};
  for (int i = 0; i < n; i++) {
    float x = (float)buf[i];
    for (int b = 0; b < 7; b++) {
      float s0 = x + coeff[b] * s1[b] - s2[b];
      s2[b] = s1[b];
      s1[b] = s0;
    }
  }
  for (int b = 0; b < 7; b++) {
    float mag2 = s1[b]*s1[b] + s2[b]*s2[b] - coeff[b]*s1[b]*s2[b];
    if (mag2 < 0.f) mag2 = 0.f;
    float mag = sqrtf(mag2) / (float)n;   // normalize by frame length

    // Auto-gain: track a slow per-band running max, normalize against it.
    if (mag > _vizMax[b]) _vizMax[b] = mag;          // jump up to new peaks
    else _vizMax[b] *= VIZ_MAX_DECAY;                // bleed down slowly
    if (_vizMax[b] < 1.f) _vizMax[b] = 1.f;          // avoid div blow-up
    float norm = mag / _vizMax[b];                   // 0..1
    if (norm > 1.f) norm = 1.f;
    float target = norm * 10.0f;                     // 0..10 bar height

    // Per-band decay smoothing: rise fast, fall slow.
    if (target > _vizBand[b]) _vizBand[b] += (target - _vizBand[b]) * VIZ_RISE;
    else                      _vizBand[b] += (target - _vizBand[b]) * VIZ_FALL;

    int v = (int)lroundf(_vizBand[b]);
    if (v < 0) v = 0; if (v > 10) v = 10;
    _vizSpec[b] = (uint8_t)v;
  }

  // ── Overall loudness envelope (0..255) ────────────────────────────
  // Peak above the adaptive baseline, scaled. Rise fast, fall slow so the
  // motion feels like a loudness meter rather than raw jitter.
  float over = (float)peak - baseline;
  if (over < 0.f) over = 0.f;
  float lvlTarget = over / 24.0f;            // ~baseline+6000 → full scale
  if (lvlTarget > 255.f) lvlTarget = 255.f;
  if (lvlTarget > _vizLevel) _vizLevel += (lvlTarget - _vizLevel) * VIZ_LVL_RISE;
  else                       _vizLevel += (lvlTarget - _vizLevel) * VIZ_LVL_FALL;
  if (_vizLevel < 0.f) _vizLevel = 0.f;

  // ── Beat-onset detection ──────────────────────────────────────────
  // Compare the current smoothed level against its slow moving average. A
  // sharp jump above avg*ratio (and above an absolute floor) is a beat,
  // debounced by a short refractory. This is animation-only and does NOT
  // feed the clap counter / approval path.
  uint32_t now = millis();
  bool refractory = (now - _vizBeatMs) < VIZ_BEAT_REFRACT_MS;
  if (!refractory &&
      _vizLevel > VIZ_BEAT_FLOOR &&
      _vizLevel > _vizLevelAvg * VIZ_BEAT_RATIO) {
    _vizBeat   = true;
    _vizBeatMs = now;
  }
  // Update the moving average AFTER the test so a beat doesn't immediately
  // raise its own reference.
  _vizLevelAvg = _vizLevelAvg * (1.0f - VIZ_AVG_ALPHA) + _vizLevel * VIZ_AVG_ALPHA;

  // Sustained-audio (music) tracking — gates the clap path in micTick(). A
  // brief clap (loud < MUSIC_SUSTAIN_MS then quiet) never sets _musicMode;
  // continuous music does, suppressing clap-driven approve/deny.
  if (_vizLevel > MUSIC_LEVEL_THRESH) {
    if (_loudSinceMs == 0) _loudSinceMs = now;
    _quietSinceMs = 0;
  } else {
    if (_quietSinceMs == 0) _quietSinceMs = now;
    if (now - _quietSinceMs > MUSIC_RELEASE_MS) _loudSinceMs = 0;
  }
  _musicMode = (_loudSinceMs != 0) && (now - _loudSinceMs > MUSIC_SUSTAIN_MS);
}

void micInit() {
  if (_initialized) return;

  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT;
  cfg.communication_format = I2S_COMM_FORMAT_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 2;
  cfg.dma_buf_len          = 128;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = false;
  cfg.fixed_mclk           = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_PIN_NO_CHANGE;
  pins.ws_io_num    = PDM_CLK_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PDM_DAT_PIN;

  if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("[mic] i2s_driver_install failed");
    return;
  }
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("[mic] i2s_set_pin failed");
    return;
  }
  i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  _initialized = true;
  Serial.println("[mic] initialized (disabled by default)");
}

void micTick() {
  if (!_enabled || !_initialized) return;

  static int16_t buf[256];
  size_t bytesRead = 0;
  if (i2s_read(I2S_PORT, buf, sizeof(buf), &bytesRead, 0) != ESP_OK) return;
  if (bytesRead == 0) return;

  int n = (int)bytesRead / 2;
  int peak = 0;
  for (int i = 0; i < n; i++) {
    int v = abs((int)buf[i]);
    if (v > peak) peak = v;
  }
  _lastPeak = peak;

  // Adaptive baseline — slow IIR of recent peaks. Only update the
  // baseline OUTSIDE the refractory window so the baseline isn't
  // contaminated by the user's intentional knocks.
  uint32_t now = millis();
  bool inRefractory = (now - _lastClapMs) <= REFRACTORY_MS;
  if (!inRefractory) {
    _baseline = _baseline * (1.0f - BASELINE_ALPHA) + (float)peak * BASELINE_ALPHA;
    if (_baseline < ABSOLUTE_FLOOR / 3.0f) _baseline = ABSOLUTE_FLOOR / 3.0f;
  }

  // Live audio-visualization feed — runs every frame off the SAME buffer,
  // purely a read path (no effect on the clap counter / approval path).
  // Only updated while sampling is enabled (we already early-returned if
  // disabled above), so callers see 0 when the mic is off.
  vizAnalyze(buf, n, peak, _baseline);

  // Onset fire rule — both gates must pass:
  bool absGate   = peak > ABSOLUTE_FLOOR;
  bool onsetGate = (float)peak > _baseline * ONSET_RATIO;
  if (absGate && onsetGate && !inRefractory && !_musicMode) {
    _lastClapMs = now;
    if (now > _windowEndMs) {
      _clapCount   = 1;
      _windowEndMs = now + WINDOW_MS;
    } else {
      _clapCount++;
    }
    Serial.printf("[mic] FIRE peak=%d baseline=%d count=%u\n",
                  peak, (int)_baseline, _clapCount);
  }
}

uint8_t micClapsConsume() {
  if (_clapCount == 0) return 0;
  if (millis() < _windowEndMs) return 0;
  uint8_t c = _clapCount;
  _clapCount = 0;
  return c;
}

void micSetEnabled(bool e) {
  _enabled = e;
  if (!e) { _clapCount = 0; _windowEndMs = 0; vizReset(); }
  Serial.printf("[mic] %s\n", e ? "enabled" : "disabled");
}

bool micEnabled() { return _enabled; }

uint32_t micLastClapMs() { return _lastClapMs; }

uint8_t micCurrentCount() {
  if (_windowEndMs == 0 || millis() > _windowEndMs) return 0;
  return _clapCount;
}

uint32_t micWindowEndMs() {
  return _windowEndMs;
}

int micPeak() {
  return _lastPeak;
}

int micBaseline() {
  return (int)_baseline;
}

// ── Live audio-visualization accessors (read-only) ───────────────────
// When sampling is disabled the _viz* state was reset to 0 by
// micSetEnabled(false), so these naturally read "no signal" without any
// extra branching cost — callers fall back to their synthetic animation.
void micSpectrum(uint8_t out[7]) {
  for (int i = 0; i < 7; i++) out[i] = _enabled ? _vizSpec[i] : 0;
}

uint8_t micLevel() {
  if (!_enabled) return 0;
  int v = (int)lroundf(_vizLevel);
  if (v < 0) v = 0; if (v > 255) v = 255;
  return (uint8_t)v;
}

bool micBeat() {
  if (!_enabled || !_vizBeat) return false;
  _vizBeat = false;   // one-shot: consume the onset
  return true;
}

// Last-action telemetry — owned by this module so callers don't have
// to wrangle their own globals to drive the listening indicator.
static uint32_t _lastActionMs = 0;
static uint8_t  _lastAction   = 0;

void micRecordAction(uint8_t kind) {
  _lastAction   = kind;
  _lastActionMs = millis();
}

uint8_t  micLastAction()   { return _lastAction; }
uint32_t micLastActionMs() { return _lastActionMs; }
