#include "mic.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <stdlib.h>

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

  // Onset fire rule — both gates must pass:
  bool absGate   = peak > ABSOLUTE_FLOOR;
  bool onsetGate = (float)peak > _baseline * ONSET_RATIO;
  if (absGate && onsetGate && !inRefractory) {
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
  if (!e) { _clapCount = 0; _windowEndMs = 0; }
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
