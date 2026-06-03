#pragma once
#include <stdint.h>

// PDM microphone capture + clap detection for the M5StickC Plus.
//
// Hardware: SPM1423 PDM mic on GPIO 0 (CLK) / GPIO 34 (DAT), decoded
// via ESP32 I2S_NUM_0 in PDM RX mode at 16 kHz / 16-bit mono.
//
// Algorithm (scaffold — tune by ear):
//   - Each micTick() pulls available DMA frames (~256 samples).
//   - Compute peak amplitude over the frame.
//   - If peak > CLAP_THRESHOLD AND we're past the refractory period,
//     register a clap and start (or extend) the count window.
//   - micClapsConsume() returns the count ONCE the window expires.
//
// Use:
//   - Call micInit() once in setup() (always safe — does nothing if
//     already initialized; sampling stays off until micSetEnabled(true)).
//   - Call micTick() every loop iteration.
//   - Poll micClapsConsume() when you want to act on a clap event.
//
// Safety:
//   - Sampling is OFF by default (micSetEnabled(false) initial state).
//     The caller must opt in via settings. This prevents a runaway
//     "cough approves the rm -rf" failure mode.
//   - Callers should only act on claps during specific UI states
//     (e.g. an active permission prompt) — not globally.

void    micInit();
void    micTick();

// Returns 1, 2, 3+ ONCE per count window after the window closes; else 0.
// Resets internal state when consumed.
uint8_t micClapsConsume();

void    micSetEnabled(bool e);
bool    micEnabled();

// Last detected clap timestamp (millis()), 0 if none yet.
// Used by the UI to flash the listening indicator on every clap so
// you can verify the mic is hearing you without needing an active
// permission prompt to test.
uint32_t micLastClapMs();

// Current in-window state (for richer UI feedback):
//   micCurrentCount(): how many knocks counted so far in the open
//     window. Returns 0 once the window has closed (consumed via
//     micClapsConsume()) or if no window is open.
//   micWindowEndMs(): millis() at which the current window will close;
//     0 if no window is open.
uint8_t  micCurrentCount();
uint32_t micWindowEndMs();

// Most recent frame peak amplitude (for live-meter UI feedback).
int micPeak();
// Current adaptive baseline (rolling mean of recent peaks excluding
// the user's intentional knocks). Used as the denominator in the
// onset ratio test, and exposed so the UI meter can normalize.
int micBaseline();

// ── Live audio-visualization feed (read-only, animation only) ────────
// These expose a lightweight spectral + loudness analysis of the live
// PDM frames so UI scenes (e.g. the Stage-5 DJ booth) can react to the
// room. They are PURELY a read path: they NEVER register a clap, change
// the count window, or touch the approval/relay path. They only update
// while sampling is enabled (micEnabled()); when disabled they read 0
// (so a scene can cheaply detect "no signal" and fall back to its own
// synthetic animation).
//
//   micSpectrum(out): fills out[0..6] with a 7-band Goertzel filter-bank
//     magnitude, each 0..10, ordered low→high frequency (~125 Hz →
//     ~6 kHz). Auto-gained against a slow running max; bars rise fast,
//     fall slow. All zero when disabled or silent.
//   micLevel(): smoothed overall loudness 0..255 — an envelope of the
//     frame peak above the adaptive baseline. 0 when disabled/silent.
//   micBeat(): returns true ONCE per detected beat onset (level jumps
//     above its moving average by a ratio), debounced by a short
//     refractory. Consuming. Always false when disabled.
void    micSpectrum(uint8_t out[7]);
uint8_t micLevel();
bool    micBeat();

// Records the outcome of the most recent mic-triggered action so the
// listening indicator can flash an outcome icon (✓ approve / ✗ deny).
// kind: 0 = none/reset, 1 = approve, 2 = deny.
void     micRecordAction(uint8_t kind);
uint8_t  micLastAction();
uint32_t micLastActionMs();
