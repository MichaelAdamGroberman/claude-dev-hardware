#pragma once
#include <stdint.h>

// Multi-species ASCII buddy renderer. Each species lives in its own
// src/buddies/<name>.cpp file and exposes 7 state functions matching
// the PersonaState enum order: sleep, idle, busy, attention, celebrate,
// dizzy, heart.
void buddyInit();
void buddyTick(uint8_t personaState);
void buddyInvalidate();
class TFT_eSPI;
void buddyRenderTo(TFT_eSPI* tgt, uint8_t personaState);
// Current render surface — sprite in home/peek, M5.Lcd in landscape clock.
// Geometric species (e.g. gr0m) call this at the top of their state
// functions so their fillCircle/fillRect/etc. land on the right surface.
TFT_eSPI* buddyTarget();
void buddySetSpecies(const char* name);
void buddySetSpeciesIdx(uint8_t idx);
void buddyNextSpecies();
void buddySetPeek(bool peek);
// Current render scale (1 = peek/secondary screens, 2 = home).
// Vector species like gr0m read this to shrink themselves on the PET
// and INFO screens so they don't overlap the panel content.
uint8_t buddyScale();
uint8_t buddySpeciesIdx();
uint8_t buddySpeciesCount();
const char* buddySpeciesName();

// Per-species state function: takes the global tickCount and renders
// the buddy + any overlays for the current state into the shared sprite.
typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];   // index by PersonaState (0=sleep .. 6=heart)
};
