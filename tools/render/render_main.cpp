// Host harness: renders the REAL gr0m character (src/buddies/gr0m.cpp via
// src/buddy.cpp) into a framebuffer and writes a PPM. Lets us preview any
// state / evolution stage / scale off-device, pixel-faithful to the firmware.
//
//   ./gr0m_render <stage 0-5> <state 0-6> <scale 1|2> <out.ppm>
//   state: 0=sleep 1=idle 2=busy 3=attention 4=celebrate 5=dizzy 6=heart
//   scale: 2=home (big), 1=peek (mini)
#include "M5StickCPlus.h"
#include "buddy.h"
#include <cstdio>
#include <cstdlib>

static const int SW = 135, SH = 240;
static uint16_t FB[SW * SH];

// Globals normally owned by main.cpp.
TFT_eSprite spr;
_M5Device   M5;
unsigned long _grender_clock_ms = 0;   // fake millis() the harness advances per frame
uint8_t  g_evoStage   = 5;
uint32_t g_dispTokens = 123456;

static void writePPM(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) { perror("fopen"); exit(1); }
  fprintf(f, "P6\n%d %d\n255\n", SW, SH);
  for (int i = 0; i < SW * SH; i++) {
    uint16_t c = FB[i];
    unsigned char r = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
    unsigned char g = (unsigned char)(((c >> 5)  & 0x3F) * 255 / 63);
    unsigned char b = (unsigned char)(( c        & 0x1F) * 255 / 31);
    fputc(r, f); fputc(g, f); fputc(b, f);
  }
  fclose(f);
}

int main(int argc, char** argv) {
  int stage = argc > 1 ? atoi(argv[1]) : 5;
  int state = argc > 2 ? atoi(argv[2]) : 1;
  int scale = argc > 3 ? atoi(argv[3]) : 2;
  const char* out = argc > 4 ? argv[4] : "out.ppm";
  int frame = argc > 5 ? atoi(argv[5]) : 0;   // animation frame (advances tickCount)

  spr.setFB(FB, SW, SH);
  buddyInit();
  buddySetSpecies("gr0m");
  g_evoStage = (uint8_t)stage;
  buddySetPeek(scale == 1);
  for (int i = 0; i < SW * SH; i++) FB[i] = 0x0000;   // device bg = black
  buddyInvalidate();
  // Advance the fake clock so buddyTick ticks the animation up to `frame`.
  for (int i = 0; i <= frame; i++) { _grender_clock_ms = (unsigned long)i * 200UL; buddyTick((uint8_t)state); }
  writePPM(out);
  return 0;
}
