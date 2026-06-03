# gr0m Level Evolution, DJ Mode & Connection Menu — Design

**Date:** 2026-05-24
**Status:** Approved (design); pending implementation plan
**Repo:** `MichaelAdamGroberman/claude-dev-hardware` (`~/Downloads/claude-desktop-buddy-main/`)

## Summary

A bundle of gr0m device enhancements built in one firmware pass:

1. **Level evolution (Part 1–2):** the character **visually evolves** from a minimal
   "just booted" form to its full current design as **lifetime token usage**
   accumulates, crossing five round-number milestones. Evolution never regresses,
   and no new art is introduced — the renderer is already an assembly of independent
   primitives, so evolution is implemented by **gating those primitives** behind the
   current stage. As a coupled requirement, the device's **approved/denied permission
   counts refresh on the same heartbeat as the token count** (same daemon→device
   push).
2. **DJ Mode (Part 3, bonus):** a toggleable full-detail "DJ booth" scene — gr0m on
   the decks with headphones, turntables, a mixer, a visor VU meter, and note
   particles, all driven by an internal ~124 BPM beat. Toggleable anytime, and
   celebrated as a one-time bonus when the character reaches full evolution
   (Stage 5).
3. **Connection menu (Part 4):** the radio (WiFi/BT/Off) and Adapter controls move
   out of the main Settings menu into their own submenu opened from the Connections
   info screen.

## Goals

1. Character starts at a basic form and progressively assembles into the full
   design as lifetime usage grows, paced to round token milestones.
2. Approved and denied counts on the device refresh in lockstep with the token
   count.
3. A toggleable full-detail DJ scene, available anytime and celebrated as the
   Stage-5 bonus.
4. Relocate the radio (WiFi/BT/Off) and Adapter controls into a dedicated submenu
   on the Connections screen.
5. Minimal, low-risk firmware diff: evolution touches no per-mood logic; nothing
   changes radio/transport *behavior* (only where the controls live).

## Non-Goals

- **Evolution:** no new sprites or art — pure gating of existing draw primitives;
  no change to mood/state logic (visor color, mouth mood per state); evolution
  never regresses once a stage is reached.
- No change to transport behavior (serial/BLE/WiFi/VPN), radio mutual-exclusion
  semantics, GPIO, or adapter-mode behavior — Part 4 only relocates the controls.
- **DJ mode** does add new draw code (turntables, mixer, VU bars, notes), but
  reuses existing primitives (head, headphones, hands, antenna, particles) wherever
  possible; it is a standalone scene, not a new mood-state, and is not subject to
  evolution gating.
- No real-audio reactivity — the DJ beat is a deterministic internal counter.

## Data Model

Today `stats().tokens` holds whatever the daemon last pushed (the **period**
figure), and `level = tokens / 50000`. That conflation makes `level` reset with the
reporting period. This design separates the concerns:

| Field | Source | Persistence | Drives |
|---|---|---|---|
| `lifetimeTokens` | daemon (computed once from transcripts, then incremented), pushed each heartbeat as `"life"` | NVS, written **only when `evoStage` advances** (rare → negligible flash wear) | `level` and `evoStage` |
| `periodTokens` | daemon push `"set"` (existing day/week/month figure) | in-memory only (existing behavior) | chest-LCD readout |
| `approved` / `denied` | daemon push `"ok"` / `"deny"` | in-memory on device; persisted daemon-side in `token_state.json` | stats screen counters |

Derived:

- `level = lifetimeTokens / 50000` — a monotonic score, shown on the stats screen,
  climbs forever (never caps).
- `evoStage` (0–5) — derived from `lifetimeTokens` crossing the milestones below.
  Caps at 5 ("fully assembled"); `level` keeps rising afterward.

**Boot behavior:** because `lifetimeTokens` is persisted to NVS on stage-up, the
device boots at the correct stage before the daemon reconnects. Between boot and
first daemon heartbeat, `periodTokens`/`approved`/`denied` show their last in-RAM
values (zero on cold boot) until the next push refreshes them.

## Evolution Ladder

Milestones are **round lifetime-token numbers**. The head cube and visor (the eyes,
which carry the mood color) are always drawn, so the pet is readable even at
Stage 0.

| Stage | Unlocks at (lifetime tokens) | Adds | Reads as |
|---|---|---|---|
| **0 Spark** | 0 | head cube + visor eyes, flat-shaded | a lone chip-head, just booted |
| **1 Frame** | 1,000,000 | + neck cube + chest cube + mouth | torso assembles, gains a mouth |
| **2 Powered** | 5,000,000 | + chest bolt + antenna LED | recognizably gr0m, powered on |
| **3 Persona** | 25,000,000 | + sunglasses | the signature attitude |
| **4 HUD** | 100,000,000 | + mood particles + chest-LCD readout + dynamic per-face lighting/specular | polished, "alive, counting" |
| **5 Ascended** | 250,000,000 | + state costumes (nightcap, headphones/laptop, party hat, confetti, heart cloud, speech bubble) | the full current design |

**Confirmed judgment calls:**

- Stage 0 is the "lone head + eyes" form (no body).
- Stages 0–3 are **flat-shaded** (single base color per cube, no per-face dynamic
  lighting); dynamic lighting + specular polish unlock at Stage 4. This gives a
  deliberately primitive early look that "polishes up" as it grows.

## Rendering Gates

The 7 mood-state functions (`doSleep`, `doIdle`, `doBusy`, `doAttention`,
`doCelebrate`, `doDizzy`, `doHeart`) all compose the same primitives in the same
z-order. Rather than edit each state, **each primitive self-gates** on its unlock
stage with an early return:

```cpp
static void drawChest3D() { if (evoStage() < 1) return; /* ... */ }
static void drawNeck3D()  { if (evoStage() < 1) return; /* ... */ }
static void drawBolt3D(...){ if (evoStage() < 2) return; /* ... */ }
static void drawAntenna3D(...){ if (evoStage() < 2) return; /* ... */ }
static void drawSunglasses3D(){ if (evoStage() < 3) return; /* ... */ }
static void drawMoodParticles(...) { if (evoStage() < 4) return; /* ... existing scale guard ... */ }
static void drawChestLCD(...) { if (evoStage() < 4) return; /* ... */ }
```

- `drawHead3D` and `drawVisor3D` are **never gated** (always present).
- `drawMouth3D`: gated whole at Stage 1 (`if (evoStage() < 1) return;`). It draws
  whatever mood the state passes — no partial "clamp to neutral" logic, since the
  mood is already computed by each state function.
- Enrichment/costume helpers (`drawNightcap`, `drawHeadphones`, `drawLaptop`,
  `drawHandsAtLaptop`, `drawPartyHat`, `drawConfetti`, `drawHeartCloud`,
  `drawSpeechBubble`) already early-return on `buddyScale() == 1`; extend that guard
  with `|| evoStage() < 5`.
- `drawCube3D` flat-shades when `evoStage() < 4`: skip `faceLighting()` and draw
  faces in the flat `baseColor` (still backface-culled). At Stage 4+ it uses the
  existing dynamic lighting path.

Net change: ~8–10 small edits localized to `src/buddies/gr0m.cpp`, plus the
`evoStage()` helper. Every mood inherits the evolution automatically.

### `evoStage()` helper

A free function in the `gr0m` namespace (or shared via `stats.h`) that maps
`stats().lifetimeTokens` to 0–5 using the milestone table. Single source of truth;
the stats-screen display and the render gates both call it.

## Approved/Denied Refresh

The daemon (`tools/cc-bridge/buddy_bridged.py`) already mediates every permission
prompt: hook → daemon → device shows prompt → user presses A/B → device reply →
daemon → hook. The daemon therefore observes each resolution and tallies:

- **approved** — count of resolutions the device approved in the current window.
- **denied** — count denied in the current window.

Scope matches the token window (day/week/month): tallies are bucketed by timestamp
and filtered to the active window, persisted in `~/.cache/claude-buddy/token_state.json`,
and zeroed by `gr0m_token_reset` alongside the token baseline.

These piggyback on the existing heartbeat push so they cannot drift out of sync with
the token count:

```jsonc
{"cmd":"tokens","set":<periodTokens>,"life":<lifetimeTokens>,"ok":<approved>,"deny":<denied>}
```

Firmware extends the `tokens` command handler (currently `statsSetTokens`) to read
the new fields into in-memory `periodTokens`, `lifetimeTokens` (+ NVS-on-stage-up),
`approved`, `denied`. The stats screen redraws all of them in one pass, so they
refresh together by construction.

## Device UI

Two surfaces show usage:

- **On the pet (chest-LCD):** the period token figure (existing behavior), now
  sourced from `periodTokens`.
- **USAGE info page (new — see Part 5):** period tokens, `OK / NO` counts, `level`,
  and `Stage x/5 →next` (e.g. `Stage 3/5 →100M`). This page also hosts the
  timespan + reset controls.

`level` continues to display as today (now lifetime-derived).

## MCP

`gr0m_status` is extended to return `level`, `evoStage`, `lifetimeTokens`,
`periodTokens`, `approved`, `denied` (in addition to existing fields). A new
`gr0m_dj(on)` tool toggles DJ mode (mirrors `gr0m_adapter`). The existing
`gr0m_token_period` and `gr0m_token_reset` tools are unchanged; the on-device Usage
menu (Part 5) drives the same daemon state via device→daemon events. An optional
`gr0m_evo` preview tool is out of scope unless requested.

## Part 3 — DJ Mode (bonus)

A toggleable, full-detail "DJ booth" scene. It is **not** a mood-state and is **not**
subject to evolution gating.

- **Runtime flag `djMode`** in `main.cpp`, mirroring `adapterMode`. The two are
  mutually exclusive (enabling one clears the other). The loop short-circuits to
  `djTick(now)` **after** `dataPoll` + transport ticks, so MCP/serial can still
  toggle it off while it runs.
- **Toggles:** a `dj` item in the Settings menu; `{"cmd":"dj","on":bool}` over any
  transport; `gr0m_dj(on)` over MCP. Exit via **BtnB** or reset (in-memory flag).
- **Internal beat:** a fixed ~124 BPM phase counter derived from `millis()`. A beat
  envelope drives the head-bob (`_yProjOff`), VU bar heights, antenna pulse,
  crossfader slide, and the right-deck scratch jitter. No mic / no real audio.
- **Scene** — lives in `gr0m.cpp` (to reach its file-static primitives), exposed as
  `gr0m::doDJ(uint32_t t)` and called from `djTick`:
  - Head + visor + **headphones** (`drawHead3D` + `drawHeadphones`), bobbing on beat.
  - **Visor → VU meter:** 5–7 vertical bars with per-bar beat envelopes; color
    cycles like club lighting (reuses the visor frame, replaces the pupil).
  - **Two turntables** (new `drawTurntable(side, angle)`): platter + vinyl grooves +
    center label + a rotating radial mark. Left deck spins steady; right deck
    scratches (angle jitters) on the beat.
  - **Mixer** (new `drawMixer(beatPhase)`): two channel faders + a crossfader that
    slides L↔R each beat + a couple of knobs.
  - **Hands on the decks** — adapt `drawHandsAtLaptop`; the right hand tracks the
    scratch.
  - **♪/♫ note particles** (new `drawNotes`, or the `drawMoodParticles` structure
    with note glyphs), antenna LED pulsing on beat, chest-LCD showing `124`.
- **Stage-5 announce:** the first time `evoStage` reaches 5, set a persistent
  `djUnlocked` NVS flag and play a one-time "DJ MODE!" celebration (confetti +
  speech bubble). DJ mode is toggleable from day one regardless — the announce is
  purely the bonus payoff.

## Part 4 — Connection-screen control submenu

Relocate the radio and adapter controls out of the main Settings menu into a
dedicated submenu opened from the **Connections** info page (page 4).

- Current Settings menu (`settingsItems[]`, `SETTINGS_N = 12`): items `bluetooth`
  (case 2) and `wifi` (case 3) are the mutually-exclusive radio toggles (each
  reboots); `adapter` is item 9.
- **New `connMenuOpen` submenu**, items: `WiFi / BT / Off / Adapter / Back`.
  - `WiFi` / `BT` set the radio (reuse the case 2/3 reboot logic); `Off` sets both
    `s.wifi` and `s.bt` false then reboots. A three-way selector is clearer than two
    independent toggles.
  - `Adapter` enters adapter mode (reuse case 9 logic).
- **Open it** from the Connections page via the action button (page-advance stays on
  the browse button); add a footer hint (e.g. `BtnA: menu`). Exact button mapping is
  verified against the input block (~lines 1650–1710) during planning.
- **Settings menu changes:** remove `bluetooth`, `wifi`, `adapter`; add `dj`
  (Part 3). New list: `brightness, sound, led, mic claps, transcript, clock rot,
  ascii pet, dj, reset, back` (10 items). Reindex `applySetting` cases accordingly.

## Part 5 — On-device Usage menu (timespan + resets)

Today the reporting period and reset are **MCP/daemon-only** (`gr0m_token_period`,
`gr0m_token_reset`, `BUDDY_TOKEN_PERIOD`). This adds on-device controls, mirroring
the Part 4 page→submenu pattern.

- **New USAGE info page** (`INFO_PAGES` 6→7) showing period tokens, `OK / NO`, level,
  and `Stage x/5 →next`. This is the home for the Device-UI usage display above.
- **Action button → `usageMenuOpen` submenu:**
  - **`span: 1 day`** — cycles **1 day / 7 days / 30 days / Full** (= `day / week /
    month / all`). On change the device emits `{"evt":"period","value":...}`; the
    label updates locally immediately.
  - **`reset count`** — tap-twice confirm; emits `{"evt":"token_reset"}` and zeroes
    the local `periodTokens` + `approved` + `denied` display at once.
  - **`reset level`** — tap-twice confirm, flagged **DESTRUCTIVE: de-evolves the
    character to Stage 0 (Spark)**. Zeroes local NVS `lifetimeTokens` + `evoStage`,
    calls `characterInvalidate()`, and emits `{"evt":"level_reset"}`.
  - **`back`**
- Both resets reuse the existing factory-reset **tap-twice "really?" confirm**, so a
  stray press can't wipe progress.

### Device ↔ daemon control channel

The daemon already reads device→host lines (it's how `op:query` collects the
`{"ack":"gpio",...}` replies), so the Usage menu emits **events** the daemon acts on:

| Event (device → daemon) | Daemon action |
|---|---|
| `{"evt":"period","value":"day\|week\|month\|all"}` | set window, recompute, persist, re-push |
| `{"evt":"token_reset"}` | set period baseline = current period total (existing reset path) |
| `{"evt":"level_reset"}` | set **lifetime baseline** = current all-tokens total |

The daemon's downlink (heartbeat push) is unchanged in shape:
`{"cmd":"tokens","set":…,"life":…,"ok":…,"deny":…}`. Because the daemon recomputes
lifetime from transcripts, **`level_reset` works via a baseline/offset** in
`token_state.json` (displayed = total − baseline) — the same mechanism the existing
token-count reset uses. The device's local NVS zeroing is for immediate feedback and
correct boot-stage before the daemon reconnects.

## Affected Components

- `src/buddies/gr0m.cpp`
  - Part 1: `evoStage()` helper, per-primitive gates, flat-shade branch in
    `drawCube3D`.
  - Part 3: `gr0m::doDJ(t)` scene + new `drawTurntable` / `drawMixer` / `drawNotes`
    + DJ VU-visor variant; reuse of `drawHeadphones` / `drawHandsAtLaptop`.
- `src/stats.h` — add `lifetimeTokens`, `periodTokens`, `approved`, `denied`,
  `djUnlocked`; extended token setter; NVS persist of `lifetimeTokens` on stage-up;
  `level` derived from `lifetimeTokens`; reset helpers (count, level).
- `src/xfer.h` — `tokens` command parses `set`/`life`/`ok`/`deny`; new `dj` command;
  emit helpers for the device→daemon `evt` messages (period / token_reset /
  level_reset).
- `src/main.cpp`
  - Part 3: `djMode` flag, loop short-circuit to `djTick`, Settings `dj` item,
    Stage-5 announce.
  - Part 4: `connMenuOpen` submenu off the Connections page; remove
    `bluetooth`/`wifi`/`adapter` from `settingsItems[]`, add `dj`, reindex
    `applySetting`.
  - Part 5: new USAGE info page (`INFO_PAGES` 6→7), `usageMenuOpen` submenu
    (timespan cycle + two tap-twice resets), usage display (stage/level/OK·NO).
  - Shared: a small menu-framework helper reused by the connection + usage submenus.
- `tools/cc-bridge/buddy_bridged.py` — maintain cached lifetime total +
  approved/denied tally; include `life`/`ok`/`deny` in the heartbeat push; handle the
  three device→daemon `evt` messages; period + lifetime baselines in
  `token_state.json`.
- `tools/cc-bridge/gr0m_mcp.py` + `tools/cc-bridge/README.md` — add `gr0m_dj`; expand
  `gr0m_status` fields; document evolution, DJ mode, the connection menu, and the
  usage menu.

## Risks / Open Questions

- **Lifetime computation cost:** computing "all" tokens from transcripts can be
  expensive for a heavy user. Mitigation: compute once at daemon startup, cache in
  `token_state.json`, then increment from session deltas rather than re-scanning.
- **`level_reset` while daemon offline:** the device-local zeroing holds until the
  daemon reconnects; if the daemon never received the event it would re-push the true
  lifetime and undo the reset. Mitigation: device persists a "level_reset pending"
  flag and replays it to the daemon on reconnect (small; flagged for the plan).
- **Screen layout:** the USAGE page lines and the submenus must fit the 135px width;
  truncate/abbreviate as needed (verify against existing layouts in `main.cpp`).
- **Input mapping for page→submenu:** opening a submenu from an info page needs a
  free button on that page; confirm BtnA/BtnB semantics in the input block
  (~lines 1650–1710) before wiring (applies to both Part 4 and Part 5).
