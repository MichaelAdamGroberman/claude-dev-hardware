#include "../buddy.h"
#include "../buddy_common.h"
#include "../stats.h"     // stats().tokens — for the chest-LCD token counter
#include "../mic.h"       // live audio-reactivity feed for the DJ scene
#include <M5StickCPlus.h>

extern TFT_eSprite spr;

// Owned by main.cpp. stats.h keeps its state file-static (can't be shared
// across translation units), so gr0m reads these mirrors instead of stats(),
// which would be a separate, never-updated copy in this .cpp.
extern uint8_t  g_evoStage;     // 0..5 character evolution stage
extern uint32_t g_dispTokens;   // period token figure for the chest-LCD

namespace gr0m {

// Current evolution stage — gates which body parts / decorations are drawn so
// the character visibly assembles itself as lifetime usage grows.
static inline uint8_t evoStage() { return ::g_evoStage; }

// Active render surface — refreshed at the top of each state function via
// buddyTarget(). In portrait/home/peek modes this is `&spr` (the off-
// screen sprite). In landscape clock mode it's `&M5.Lcd` (direct draw).
// All geometric helpers below route through this pointer so the robot
// lands on the correct surface regardless of mode.
static TFT_eSPI* _t = &spr;

// ── Accelerometer-driven parallax ────────────────────────────────────
// Each state function reads the IMU once and stashes the resulting
// tilt offsets here. Drawing helpers add these offsets to specific
// elements (visor "pupil", antenna sway, shadow position, particle drift)
// to create a fake-3D / parallax-depth effect as the device is tilted.
//   _tiltX:  -4..+4   negative = left tilt
//   _tiltY:  -4..+4   negative = forward (away from user) tilt
//   _gravX:  -2..+2   gravity component along screen x — drives particle drift
static int8_t _tiltX = 0;
static int8_t _tiltY = 0;
static int8_t _gravX = 0;

// Tilt in DEGREES (linear g·90) for the SPEC behaviours.
static float _tiltDegX = 0;
static float _tiltDegY = 0;

static float _headRotV = 0, _headRotP = 0;   // #tilt-bobble head spring

// #tilt-gravity detach: ATTACHED→FALLING >35°, back <25° (hyst).
// Only two states are reachable — updateItemDetach toggles ATTACHED↔FALLING.
enum ItemState : uint8_t { ITEM_ATTACHED = 0, ITEM_FALLING = 1 };
static ItemState _hatState   = ITEM_ATTACHED;   // party hat
static ItemState _shadeState = ITEM_ATTACHED;   // sunglasses

static int8_t _clamp(float v, int lo, int hi) {
  int iv = (int)(v + (v >= 0 ? 0.5f : -0.5f));
  if (iv < lo) iv = lo;
  if (iv > hi) iv = hi;
  return (int8_t)iv;
}

static void updateItemDetach(ItemState& st) {
  float a = fabsf(_tiltDegX);
  if (st == ITEM_ATTACHED) { if (a > 35.0f) st = ITEM_FALLING; }
  else                     { if (a < 25.0f) st = ITEM_ATTACHED; }
}

// Forward decl — buildRotation body lives in the 3D pipeline section.
static void buildRotation(float yaw, float pitch);
static int _xProjOff = 0;   // #tilt-bobble proj-X (declared early)

static void readTilt() {
  float ax = 0, ay = 0, az = 0;
  M5.Imu.getAccelData(&ax, &ay, &az);
  _xProjOff = 0;   // #tilt-bobble re-sets this in idle each frame

  // Low-pass IIR smoothing — kills hand-tremor jitter while staying
  // responsive (~100 ms time constant at 5 fps).
  static float axS = 0, ayS = 0;
  axS = axS * 0.7f + ax * 0.3f;
  ayS = ayS * 0.7f + ay * 0.3f;
  // Wider range (±8) so the pseudo-3D head rotation reads clearly.
  // Symmetric coefficients on both axes — up/down pitch matches the
  // strength of left/right yaw so the head visibly looks up/down when
  // the device is tilted forward/back, not just side-to-side.
  _tiltX = _clamp(axS *  8.0f, -8, 8);
  _tiltY = _clamp(ayS *  8.0f, -8, 8);
  _gravX = _clamp(axS *  3.0f, -3, 3);
  // Degree tilt (linear g·90; |deg|≤90).
  float gx = axS; if (gx >  1.0f) gx =  1.0f; if (gx < -1.0f) gx = -1.0f;
  float gy = ayS; if (gy >  1.0f) gy =  1.0f; if (gy < -1.0f) gy = -1.0f;
  _tiltDegX = gx * 90.0f;
  _tiltDegY = gy * 90.0f;
  updateItemDetach(_hatState);    // #tilt-gravity detach
  updateItemDetach(_shadeState);
  // Build the actual 3D rotation matrix used by the cube renderer.
  // Yaw from ax (head turns left/right when device tilts side-to-side);
  // pitch from ay (head looks up/down when device pitches forward/back).
  // ±0.7 rad ≈ ±40° at max tilt — strong but not disorienting.
  float yaw   = -axS * 0.7f;
  float pitch =  ayS * 0.7f;
  buildRotation(yaw, pitch);
}

// Rigid-body face-plane offset. Every head-attached element (chassis,
// visor, glasses, mouth) adds this so they rotate TOGETHER
// as a single 3D plane. The chest stays anchored — head appears to
// pivot on the neck.
static int faceOffX() { return _tiltX; }       // ±8 px lateral shift
static int faceOffY() { return _tiltY / 2; }   // ±3 px vertical (forward/back lean)

// #tilt-bobble — step the head spring (k=0.15, damp=0.85 per SPEC) into
// _xProjOff so the head wobbles laterally vs the drawn body. `active` gates
// the drive (idle, |tilt|>10°). Call AFTER chest.
static void applyHeadBobble(bool active) {
  float target = active ? (_tiltDegX * 0.12f) : 0.0f;
  float force = (target - _headRotP) * 0.15f;
  _headRotV = (_headRotV + force) * 0.85f;
  _headRotP += _headRotV;
  _xProjOff = (int)(_headRotP + (_headRotP >= 0 ? 0.5f : -0.5f));
}

// Canvas anchor — head center on screen. Constants need to be visible
// to the 3D projection code below, so they're hoisted above the body
// part definitions where they're used.
static const int HX = 67;
static const int HY = 55;
static const int HW = 54;
static const int HH = 46;

// ── True-3D rendering pipeline ──────────────────────────────────────
// gr0m is a hierarchical 3D figure (head cube + chest cube + poles)
// rotated by a real 3×3 matrix derived from accelerometer yaw + pitch,
// projected through a perspective transform, drawn with backface-culled
// filled quads. The face decorations (visor, glasses, mouth, bolt)
// live on the front-face plane (object-space z = +size) so they rotate
// with the head and vanish when the head turns past 90°.
struct V3 { float x, y, z; };
struct V2 { int x, y; };

static float _rot[9];          // 3×3 rotation matrix
static int   _yProjOff = 0;    // bob/jump offset applied at projection time
// _xProjOff (#tilt-bobble lateral shift) is declared up in the tilt section.

// ── DJ-scene head transform ─────────────────────────────────────────
// The composed DJ scene packs the gr0m head into a TOP band (~y22–66) so the
// decks + EQ get the rest of the buddy zone. Rather than re-author every
// face primitive, we post-scale the normal HOME projection about the head
// anchor and re-center it on a DJ target row. _djHeadScale=1 + the default
// center is the identity, so every NON-DJ state projects byte-for-byte as
// before. djMusicScene sets these for the head draws and clears them after.
static float _djHeadScale = 1.0f;     // <1 shrinks the head
static int   _djHeadCenterY = HY;     // screen-space target for the head center
static inline int djHeadMapY(int y) {
  // Map a HOME-projected screen-Y into the shrunk/re-centered DJ head band.
  if (_djHeadScale == 1.0f && _djHeadCenterY == HY) return y;
  return _djHeadCenterY + (int)lroundf((y - HY) * _djHeadScale);
}
static inline int djHeadMapX(int x) {
  if (_djHeadScale == 1.0f) return x;
  return HX + (int)lroundf((x - HX) * _djHeadScale);
}

static void buildRotation(float yaw, float pitch) {
  float cy = cosf(yaw),   sy = sinf(yaw);
  float cp = cosf(pitch), sp = sinf(pitch);
  // R = Rx(pitch) * Ry(yaw)
  _rot[0] = cy;        _rot[1] = 0;    _rot[2] = -sy;
  _rot[3] = sy * sp;   _rot[4] = cp;   _rot[5] = cy * sp;
  _rot[6] = sy * cp;   _rot[7] = -sp;  _rot[8] = cy * cp;
}

static V3 rotateV(V3 v) {
  return { _rot[0]*v.x + _rot[1]*v.y + _rot[2]*v.z,
           _rot[3]*v.x + _rot[4]*v.y + _rot[5]*v.z,
           _rot[6]*v.x + _rot[7]*v.y + _rot[8]*v.z };
}

// Perspective projection: camera at z = +CAM_DIST looking toward -z.
// Object-space origin maps to (HX, HY + _yProjOff) on screen.
//
// PEEK MODE (buddyScale() == 1): on the PET/INFO screens the panel
// content starts at y=70, so we halve FOCAL and shift the head anchor
// up to y=32 so the bot tucks into the upper strip and doesn't bleed
// over the panel. On the home screen (scale 2) we use the full 97 px
// focal length and the original HY=55 anchor.
static V2 projectV(V3 v) {
  const float CAM_DIST = 90.0f;
  bool peek = (buddyScale() == 1);
  const float FOCAL = peek ? 50.0f : 97.0f;
  const int   ANCHOR_Y = peek ? 32 : HY;
  float z = v.z + CAM_DIST;
  if (z < 1.0f) z = 1.0f;
  int sx = (int)(HX + _xProjOff + v.x * FOCAL / z);
  int sy = (int)(ANCHOR_Y + _yProjOff + v.y * FOCAL / z);
  // DJ scene packs the head into a top band — post-scale about the anchor.
  return { djHeadMapX(sx), djHeadMapY(sy) };
}

// Rotate + project in one call — common pattern.
static V2 rp(V3 v) { return projectV(rotateV(v)); }

// ── Peek (mini) coordinate mapping ───────────────────────────────────
// The 2D accessory/particle/costume helpers below are authored in the
// HOME layout (head center HX,HY; full-size pixel offsets). In peek mode
// the body is re-projected smaller and higher (see projectV: anchor
// y=32, focal 50 vs HY/97), but these helpers historically didn't get
// that transform, so they were gated off and the mini lost its mood and
// evolution gear. These map a HOME screen coordinate / length onto the
// peek body so hats, costumes, mood particles and the chest HUD ride the
// shrunk character (and stay above the y=70 stats panel) instead of
// overflowing it. At home scale (2) they are the identity, so the home
// screen renders byte-for-byte as before.
static const float PEEK_K = 50.0f / 97.0f;   // focal ratio, matches projectV()
static const int   PEEK_ANCHOR_Y = 32;       // peek head-center y, matches projectV()
static inline int pkX(int xh) {
  return (buddyScale() == 1) ? HX + (int)((xh - HX) * PEEK_K) : xh;
}
static inline int pkY(int yh) {
  return (buddyScale() == 1) ? PEEK_ANCHOR_Y + (int)((yh - HY) * PEEK_K) : yh;
}
static inline int pkS(int len) {              // scale a length / radius
  return (buddyScale() == 1) ? (int)(len * PEEK_K + 0.5f) : len;
}

// ════════════════════════════════════════════════════════════════════
//   gr0m — vector ROBOT mascot
// ────────────────────────────────────────────────────────────────────
//   Composition:
//     • boxy rounded head with panel-line detail and antenna LED
//     • wide horizontal visor stripe — its color is the mood channel
//     • speaker-grille mouth
//     • slim neck + chest panel with status LED
//
//   Default loadout (every state EXCEPT BUSY):
//     • opaque black sunglasses overlaid across the visor
//
//   BUSY state — gr0m gets serious:
//     • sunglasses come OFF — the bare cyan/green visor is exposed
//     • the discarded sunglasses tumble at the upper-left corner
//     • grimace mouth, antenna LED blinks
//
//   Mood particles (left side of canvas): red hearts rising → fade
//   into small neutral sparkles at the half-life of their flight.
// ════════════════════════════════════════════════════════════════════

// Palette (RGB565)
static const uint16_t CHASSIS    = 0xC638;  // brushed-steel body
static const uint16_t CHASSIS_SH = 0x8410;  // chassis shadow / panel-line
static const uint16_t CHASSIS_HI = 0xEF7D;  // chassis highlight
static const uint16_t INK        = 0x0000;  // sunglasses lens fill
static const uint16_t STEEL      = 0x73AE;  // outlines + lens frame
static const uint16_t SPECULAR      = 0xFFFF;
static const uint16_t CRIMSON    = 0xF800;  // antenna LED, accents
static const uint16_t HEART_RED  = 0xF810;
static const uint16_t MOTE_TWINKLE = 0xFCB9;  // soft pink-white — mood-mote fade-out
                                              // terminus (was stark white SPECULAR,
                                              // which blinked pink↔white beside the bot)
static const uint16_t VISOR_IDLE = 0x05FF;  // cyan
static const uint16_t VISOR_BUSY = 0x07E0;  // green (focused mode)
static const uint16_t VISOR_ALERT= 0xF800;  // red
static const uint16_t VISOR_LOVE = 0xF81F;  // magenta
static const uint16_t VISOR_OFF  = 0x18C3;  // dim slate

// (The Stage-5 human-costume palette lives next to its primitives further
//  down — see the HC_* block before drawHumanHoodie.)

// ── Particle primitives ─────────────────────────────────────────────

static void drawHeart(int x, int y, uint16_t c) {
  if (buddyScale() == 1) {               // tiny heart for the mini character
    _t->fillCircle(x - 1, y - 1, 1, c);
    _t->fillCircle(x + 1, y - 1, 1, c);
    _t->fillTriangle(x - 2, y, x + 2, y, x, y + 2, c);
    return;
  }
  _t->fillCircle(x - 2, y - 1, 2, c);
  _t->fillCircle(x + 2, y - 1, 2, c);
  _t->fillTriangle(x - 3, y, x + 3, y, x, y + 4, c);
}

// Small neutral sparkle/star — used as the fade-out terminus for the
// mood-particle hearts. A 4-point twinkle, sized for home vs. mini.
static void drawSparkle(int x, int y, uint16_t c) {
  if (buddyScale() == 1) {               // tiny sparkle for the mini character
    _t->drawPixel(x, y, c);
    _t->drawPixel(x - 1, y, c);
    _t->drawPixel(x + 1, y, c);
    _t->drawPixel(x, y - 1, c);
    _t->drawPixel(x, y + 1, c);
    return;
  }
  _t->drawFastHLine(x - 2, y, 5, c);
  _t->drawFastVLine(x, y - 2, 5, c);
  _t->drawPixel(x - 1, y - 1, c);
  _t->drawPixel(x + 1, y + 1, c);
}

// ── Robot body parts ────────────────────────────────────────────────

// Soft drop shadow beneath the robot — offset by tilt so it tracks the
// "light source" direction, giving a fake-3D "lifted off screen" feel.
static void drawShadow(int yOff) {
  int sx = pkX(HX + _tiltX * 2);
  int sy = pkY(HY + 60 + yOff);
  // 3-ring soft shadow, darkest at center
  _t->fillEllipse(sx, sy + pkS(2), pkS(26), pkS(3), 0x10A2);
  _t->fillEllipse(sx, sy + pkS(1), pkS(22), pkS(2), 0x20C3);
  _t->fillEllipse(sx, sy,          pkS(18), pkS(2), 0x2965);
}

// Antenna with LED bulb. ledColor=0 means LED off (dark).
// The antenna sways OPPOSITE to tilt (inertia feel — like the antenna
// is on a flexible mount and lags behind the body's motion).
static void drawAntenna(uint16_t ledColor, int yOff) {
  int fx = faceOffX();
  int fy = faceOffY();
  int baseX = HX + fx;                    // base rides the face plane
  int topY  = HY - HH/2 + yOff + fy;
  // Sway in OPPOSITE direction relative to face — inertia feel
  int sway = -_tiltX;
  // Curved stalk — 3 segments approximating a bend
  _t->drawLine(baseX,       topY,      baseX,            topY - 3, CHASSIS_SH);
  _t->drawLine(baseX + 1,   topY,      baseX + 1,        topY - 3, CHASSIS_SH);
  _t->drawLine(baseX,       topY - 3,  baseX + sway / 2, topY - 6, CHASSIS_SH);
  _t->drawLine(baseX + 1,   topY - 3,  baseX + 1 + sway / 2, topY - 6, CHASSIS_SH);
  _t->drawLine(baseX + sway / 2,   topY - 6, baseX + sway, topY - 9, CHASSIS_SH);
  _t->drawLine(baseX + 1 + sway / 2, topY - 6, baseX + 1 + sway, topY - 9, CHASSIS_SH);
  // LED ball at the swayed tip
  int ledX = baseX + sway;
  int ledY = topY - 11;
  if (ledColor) {
    _t->fillCircle(ledX, ledY, 3, ledColor);
    _t->drawCircle(ledX, ledY, 3, SPECULAR);
    _t->drawPixel(ledX - 1, ledY - 1, SPECULAR);  // specular highlight
    // Outer glow halo
    uint16_t halo = (ledColor >> 2) & 0x39E7;
    _t->drawCircle(ledX, ledY, 4, halo);
  } else {
    _t->drawCircle(ledX, ledY, 3, CHASSIS_SH);
  }
}

// ════════════════════════════════════════════════════════════════════
//   3D PRIMITIVES — render rotated geometry to the sprite
// ════════════════════════════════════════════════════════════════════

// Cross product z-component of 2D vectors (edge1 × edge2). Positive = CCW
// = face is back-facing in screen space (camera looks toward -z, but
// after projection the convention flips). We cull faces where the
// screen-space winding is clockwise, which corresponds to back faces.
static int screenWinding(V2 a, V2 b, V2 c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Draw a 3D filled quad with edge outline. Backface-culled via screen
// winding. Quad winding must be CCW when viewed from outside the cube.
static void drawQuad3D(V3 a, V3 b, V3 c, V3 d, uint16_t fill, uint16_t edge) {
  V2 pa = rp(a), pb = rp(b), pc = rp(c), pd = rp(d);
  if (screenWinding(pa, pb, pc) <= 0) return;  // backface cull
  _t->fillTriangle(pa.x, pa.y, pb.x, pb.y, pc.x, pc.y, fill);
  _t->fillTriangle(pa.x, pa.y, pc.x, pc.y, pd.x, pd.y, fill);
  _t->drawLine(pa.x, pa.y, pb.x, pb.y, edge);
  _t->drawLine(pb.x, pb.y, pc.x, pc.y, edge);
  _t->drawLine(pc.x, pc.y, pd.x, pd.y, edge);
  _t->drawLine(pd.x, pd.y, pa.x, pa.y, edge);
}

// Forward decls — lighting helpers defined below.
static uint8_t faceLighting(float nx, float ny, float nz);
static uint16_t shade(uint16_t c, uint8_t lit);

// Render a cuboid with a single base color, dynamically lit per-face
// using face normals × the global light direction. The face you can
// see is shaded based on how much it faces the light, which gives a
// rich volumetric appearance as the cube rotates.
//
// Position is the object-space CENTER (the rotation pivot).
static void drawCube3D(V3 center, float sx, float sy, float sz,
                       uint16_t baseColor, uint16_t edge) {
  // 8 vertices in object space (centered on origin, then translated)
  V3 v[8];
  for (int i = 0; i < 8; i++) {
    v[i] = { ((i & 1) ? sx : -sx) + center.x,
             ((i & 2) ? sy : -sy) + center.y,
             ((i & 4) ? sz : -sz) + center.z };
  }
  // Face normals (object space, before rotation)
  struct F { int a, b, c, d; float nx, ny, nz; };
  static const F faces[6] = {
    { 4, 5, 7, 6,  0,  0,  1 },  // front
    { 1, 0, 2, 3,  0,  0, -1 },  // back
    { 0, 4, 6, 2, -1,  0,  0 },  // left
    { 5, 1, 3, 7,  1,  0,  0 },  // right
    { 0, 1, 5, 4,  0, -1,  0 },  // top (y=-sy in our coord system)
    { 6, 7, 3, 2,  0,  1,  0 },  // bottom
  };
  // Stages 0-3 are flat-shaded (single base color per face) for a deliberately
  // primitive look; dynamic per-face lighting unlocks at Stage 4 (HUD).
  bool flat = evoStage() < 4;
  for (int f = 0; f < 6; f++) {
    uint16_t fill = flat
        ? baseColor
        : shade(baseColor, faceLighting(faces[f].nx, faces[f].ny, faces[f].nz));
    drawQuad3D(v[faces[f].a], v[faces[f].b], v[faces[f].c], v[faces[f].d], fill, edge);
  }
}

// Check if the front face of the head is visible (i.e. face decorations
// should render). Front face normal in object space is +z; after
// rotation we check if its z-component is positive (toward camera).
static bool frontFaceVisible() {
  // Front normal (0,0,1) rotated: just the third column of R = (m[2], m[5], m[8])
  return _rot[8] > 0.0f;
}

// Project a point in object-space FRONT FACE coordinates (z = +halfDepth)
// to screen. Used to place visor/glasses/mouth on the rotated face.
static V2 frontFacePoint(float x, float y, float halfDepth) {
  return rp({ x, y, halfDepth });
}

// Draw a 3D pole (line in 3D between two object-space endpoints).
static void drawPole3D(V3 a, V3 b, uint16_t color) {
  V2 pa = rp(a), pb = rp(b);
  _t->drawLine(pa.x, pa.y, pb.x, pb.y, color);
  // Thicker line via parallel offset (1 px each direction)
  _t->drawLine(pa.x + 1, pa.y, pb.x + 1, pb.y, color);
  _t->drawLine(pa.x, pa.y + 1, pb.x, pb.y + 1, color);
}

// ── Dynamic lighting ─────────────────────────────────────────────────
// Compute a per-face brightness multiplier in 0..255 based on how much
// the rotated face normal faces an implied light source above + behind
// the camera. Used to dim/brighten the cube faces as it rotates.
static uint8_t faceLighting(float nx, float ny, float nz) {
  // Light vector (normalized): pointing from upper-front (-y, +z dir)
  static const float LX = 0.2f, LY = -0.6f, LZ = 0.8f;  // ≈ unit length
  // Rotate the local face normal by the global rotation matrix
  float rx = _rot[0]*nx + _rot[1]*ny + _rot[2]*nz;
  float ry = _rot[3]*nx + _rot[4]*ny + _rot[5]*nz;
  float rz = _rot[6]*nx + _rot[7]*ny + _rot[8]*nz;
  float dot = rx*LX + ry*LY + rz*LZ;
  if (dot < 0) dot = 0;
  // Bias so even shadowed faces aren't pitch black
  float v = 0.45f + 0.55f * dot;
  if (v > 1.0f) v = 1.0f;
  return (uint8_t)(v * 255.0f);
}

// Apply a 0..255 brightness multiplier to an RGB565 color.
static uint16_t shade(uint16_t c, uint8_t lit) {
  uint8_t r = (c >> 11) & 0x1F;
  uint8_t g = (c >>  5) & 0x3F;
  uint8_t b =  c        & 0x1F;
  r = (uint8_t)((r * lit) >> 8);
  g = (uint8_t)((g * lit) >> 8);
  b = (uint8_t)((b * lit) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// True-3D head cube. Drawn with the rotation matrix from readTilt() so
// it actually rotates in 3D as the device tilts (yaw from ax, pitch
// from ay). Backface-culled — the face you see depends on rotation.
// Each face has a different shading: front lightest, back darkest,
// sides + top brighter for the implied overhead light source.
static void drawHead3D() {
  drawCube3D({0, 0, 0}, 22.0f, 20.0f, 22.0f, CHASSIS, CHASSIS_SH);
}

// 3D chest cube — smaller, sits below the head. Front face hosts the
// lightning-bolt brand logo. Lit dynamically like the head.
static void drawChest3D() {
  if (evoStage() < 1) return;            // Stage 1 (Frame): torso appears
  drawCube3D({0, 36, 0}, 22.0f, 9.0f, 12.0f, CHASSIS, CHASSIS_SH);
}

// Small 3D neck cube between head and chest. Doesn't need lighting
// detail — small enough to read as a connector.
static void drawNeck3D() {
  if (evoStage() < 1) return;            // Stage 1 (Frame): neck connector
  drawCube3D({0, 23, 0}, 4.0f, 4.0f, 4.0f, CHASSIS_SH, CHASSIS_SH);
}

// 3D antenna — pole rising from top of head with a glowing LED ball.
// The antenna is PRESENT from Stage 0 (Core): the stalk + a dark, unlit LED
// always draw. The LED only LIGHTS from Stage 2 (Powered) onward — below that
// the requested ledColor is forced off so stages 0-1 show a bare antenna.
static void drawAntenna3D(uint16_t ledColor) {
  if (evoStage() < 2) ledColor = 0;      // Stage 0-1 (Core/Frame): antenna present, LED dark
  V3 base = { 0, -22, 0 };
  V3 tip  = { 0, -36, 0 };
  drawPole3D(base, tip, CHASSIS_SH);
  V2 p = rp(tip);
  int lr = pkS(3); if (lr < 1) lr = 1;   // LED radius — scaled to the mini antenna
  if (ledColor) {
    _t->fillCircle(p.x, p.y, lr, ledColor);
    _t->drawCircle(p.x, p.y, lr, SPECULAR);
    _t->drawCircle(p.x, p.y, lr + 1, (ledColor >> 2) & 0x39E7);  // halo
    _t->drawPixel(p.x - 1, p.y - 1, SPECULAR);
  } else {
    _t->drawCircle(p.x, p.y, lr, CHASSIS_SH);
  }
}

// Project a 2D point on the front face plane (z = +22) to screen,
// for face decorations (visor, glasses, mouth) that ride the rotation.
static V2 onFace(float x, float y) {
  return rp({ x, y, 22.0f });
}

// 3D visor — perspective-correct quad on the front face. Stretches and
// skews as the head rotates.
static void drawVisor3D(uint16_t color) {
  if (!frontFaceVisible()) return;
  V2 tl = onFace(-18, -7);
  V2 tr = onFace( 18, -7);
  V2 br = onFace( 18,  3);
  V2 bl = onFace(-18,  3);
  // Filled quad (2 triangles)
  _t->fillTriangle(tl.x, tl.y, tr.x, tr.y, br.x, br.y, color);
  _t->fillTriangle(tl.x, tl.y, br.x, br.y, bl.x, bl.y, color);
  // Dark frame outline
  _t->drawLine(tl.x, tl.y, tr.x, tr.y, CHASSIS_SH);
  _t->drawLine(tr.x, tr.y, br.x, br.y, CHASSIS_SH);
  _t->drawLine(br.x, br.y, bl.x, bl.y, CHASSIS_SH);
  _t->drawLine(bl.x, bl.y, tl.x, tl.y, CHASSIS_SH);
  // #tilt-pupil — both axes off the degree tilt. pupilX∈±6, pupilY∈±3.
  float puX = _tiltDegX * 0.4f; if (puX >  6) puX =  6; if (puX < -6) puX = -6;
  float puY = _tiltDegY * 0.2f; if (puY >  3) puY =  3; if (puY < -3) puY = -3;
  V2 pup = onFace(puX, -2 + puY);
  int pw = pkS(3), ph = pkS(4); if (pw < 1) pw = 1; if (ph < 1) ph = 1;
  _t->fillRect(pup.x - pw / 2, pup.y - ph / 2, pw, ph, SPECULAR);
}

// 3D sunglasses — TWO BIG BLACK FILLED CIRCLE LENSES over the two eye zones,
// with the CYAN VISOR STRIPE VISIBLE BETWEEN AND UNDER the lenses, a steel
// bridge across the nose, and a white catchlight on each lens. Lens centers
// are projected onto the rotated head face so the shades track the 3D head.
//
// Faithful to gr0m.jsx <Sunglasses>: two r=9 lenses at canvas x=55/79 (≡ ±12
// from the face center) over the eye zones, an 8px steel bridge, and a white
// catchlight speck on each lens — but tuned so the cyan visor band reads
// clearly between the lenses (the bridge gap) and along the bottom edge, which
// is the whole point of the look (visor color = mood channel).
static void drawSunglasses3D(uint16_t visorColor = VISOR_IDLE) {
  if (evoStage() < 3) return;            // Stage 3 (Persona): shades on
  if (!frontFaceVisible()) return;
  // #tilt-gravity detached: slide off down-side + drop (rot omitted).
  float sx = 0, sy = 0;
  if (_shadeState == ITEM_FALLING) {
    float s = fabsf(_tiltDegX) - 35.0f; if (s > 25.0f) s = 25.0f;
    sx = s * (_tiltDegX > 0 ? 1 : -1);
    sy = fabsf(sx) * 0.6f;
  }
  // Visor band is object-space y∈[-7,3] (10 tall, mid ≈ -2). Lens centers sit
  // on that mid-line, pushed apart to ±11 so a CYAN GAP shows between them and
  // the band peeks out top + bottom. Big lenses (r≈8) cover each eye zone.
  V2 lL = onFace(-11 + sx, -2 + sy);
  V2 lR = onFace( 11 + sx, -2 + sy);
  int lr = pkS(8); if (lr < 2) lr = 2;   // lens radius — big, scaled for the mini face

  // 1) Big black filled lenses over the two eye zones.
  _t->fillCircle(lL.x, lL.y, lr, INK);
  _t->fillCircle(lR.x, lR.y, lr, INK);

  // 2) Re-expose the CYAN VISOR between + under the lenses. The lenses are big
  //    and overlap toward center, so without this the cyan would be buried.
  //    Bridge gap: a vertical cyan sliver between the inner lens edges.
  //    Bottom sliver: a thin cyan band under both lenses along the visor base.
  //    (We re-paint visor-colored pixels instead of moving the lenses so the
  //    lenses stay big + round, as in the design.)
  for (float gy = -6.0f; gy <= 1.0f; gy += 1.0f) {        // bridge gap column
    V2 g = onFace(0.0f + sx, gy + sy);
    _t->drawPixel(g.x,     g.y, visorColor);
    _t->drawPixel(g.x + 1, g.y, visorColor);
    _t->drawPixel(g.x - 1, g.y, visorColor);
  }
  for (float bx = -16.0f; bx <= 16.0f; bx += 1.0f) {       // bottom visor sliver
    V2 bpt = onFace(bx + sx, 2.5f + sy);
    _t->drawPixel(bpt.x, bpt.y, visorColor);
  }

  // 3) Steel frames — double-stroked rings for a chunky aviator silhouette.
  _t->drawCircle(lL.x, lL.y, lr, STEEL);
  _t->drawCircle(lL.x, lL.y, lr + 1, STEEL);
  _t->drawCircle(lR.x, lR.y, lr, STEEL);
  _t->drawCircle(lR.x, lR.y, lr + 1, STEEL);

  // 4) Steel bridge across the nose — a short bar between the inner lens edges
  //    (sits just above the cyan bridge gap so the gap still reads).
  V2 brL = onFace(-3 + sx, -3 + sy);
  V2 brR = onFace( 3 + sx, -3 + sy);
  _t->drawLine(brL.x, brL.y, brR.x, brR.y, STEEL);
  _t->drawLine(brL.x, brL.y - 1, brR.x, brR.y - 1, STEEL);

  // 5) White catchlight speck on each lens — upper-left of each, as in the SVG.
  V2 hL = onFace(-14 + sx, -5 + sy);
  V2 hR = onFace(  8 + sx, -5 + sy);
  _t->fillRect(hL.x, hL.y, pkS(2) < 1 ? 1 : pkS(2), 1, SPECULAR);
  _t->fillRect(hR.x, hR.y, pkS(2) < 1 ? 1 : pkS(2), 1, SPECULAR);
}

// 3D mouth — small projected shape on the front face.
static void drawMouth3D(int mood) {
  if (evoStage() < 1) return;            // Stage 1 (Frame): gains a mouth
  if (!frontFaceVisible()) return;
  V2 mL = onFace(-8, 14);
  V2 mR = onFace( 8, 14);
  V2 mC = onFace( 0, 14);
  switch (mood) {
    case 0: _t->drawLine(mL.x, mL.y, mR.x, mR.y, CHASSIS_SH); break;
    case 1: _t->drawLine(mL.x, mL.y, mR.x, mR.y, CHASSIS_SH);
            _t->drawLine(mL.x, mL.y + 1, mR.x, mR.y + 1, CHASSIS_SH); break;
    case 2: {
      V2 mB = onFace(0, 16);
      _t->drawLine(mL.x, mL.y - 1, mB.x, mB.y, CHASSIS_SH);
      _t->drawLine(mB.x, mB.y, mR.x, mR.y - 1, CHASSIS_SH);
      break;
    }
    case 3: {  // grimace zigzag
      for (int x = -7; x <= 5; x += 2) {
        V2 a = onFace(x, 13 + (x & 2 ? 1 : -1));
        V2 b = onFace(x + 2, 13 + (x & 2 ? -1 : 1));
        _t->drawLine(a.x, a.y, b.x, b.y, CHASSIS_SH);
      }
      break;
    }
    case 4: {  // O shouting
      int mr = pkS(4); if (mr < 1) mr = 1;
      _t->fillCircle(mC.x, mC.y + 1, mr, INK);
      _t->drawCircle(mC.x, mC.y + 1, mr, CHASSIS_SH);
      break;
    }
    case 6: {  // X
      V2 a = onFace(-4, 12); V2 b = onFace( 4, 16);
      V2 c = onFace(-4, 16); V2 d = onFace( 4, 12);
      _t->drawLine(a.x, a.y, b.x, b.y, CHASSIS_SH);
      _t->drawLine(c.x, c.y, d.x, d.y, CHASSIS_SH);
      break;
    }
    case 7: {  // bashful — small smile + blush
      _t->drawLine(mL.x + 4, mL.y, mR.x - 4, mR.y, CHASSIS_SH);
      V2 blL = onFace(-9, 16), blR = onFace(9, 16);
      _t->fillCircle(blL.x, blL.y, 1, HEART_RED);
      _t->fillCircle(blR.x, blR.y, 1, HEART_RED);
      break;
    }
    default:
      _t->drawLine(mL.x, mL.y, mR.x, mR.y, CHASSIS_SH);
  }
}

// 3D lightning bolt on the chest's front face. Object-space y = +36
// (chest center), z = +12 (chest front plane).
static void drawBolt3D(uint16_t fill) {
  if (evoStage() < 2) return;            // Stage 2 (Powered): brand bolt lights
  // Check the chest's front face is visible (same check as head front)
  if (!frontFaceVisible()) return;
  V3 chestFront = { 0, 0, 12 };  // we'll add chest center y inline
  V2 p1 = rp({ -3, 32, 12 });
  V2 p2 = rp({  3, 32, 12 });
  V2 p3 = rp({ -1, 36, 12 });
  V2 p4 = rp({  3, 36, 12 });
  V2 p5 = rp({ -3, 40, 12 });
  uint16_t glow = (fill >> 1) & 0x7BEF;
  // Halo (slightly larger triangles in dimmer color)
  _t->fillTriangle(p1.x, p1.y - 1, p2.x, p2.y - 1, p3.x, p3.y, glow);
  _t->fillTriangle(p2.x + 1, p2.y, p3.x, p3.y, p4.x + 1, p4.y, glow);
  _t->fillTriangle(p3.x, p3.y, p4.x + 1, p4.y, p5.x, p5.y + 1, glow);
  // Bolt fill
  _t->fillTriangle(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y, fill);
  _t->fillTriangle(p2.x, p2.y, p3.x, p3.y, p4.x, p4.y, fill);
  _t->fillTriangle(p3.x, p3.y, p4.x, p4.y, p5.x, p5.y, fill);
  // Leading edge highlight
  _t->drawLine(p2.x, p2.y, p3.x, p3.y, SPECULAR);
}

// Legacy 2D drawHead — kept for the pseudo-3D code path that some
// state code may still reference. Will be removed once all states
// switch to drawHead3D.
static void drawHead(int yOff) {
  int fx = faceOffX();
  int fy = faceOffY();
  int x = HX - HW/2 + fx;
  int y = HY - HH/2 + yOff + fy;

  // ── 3D SIDE RIM ──  The "back" side of the head, exposed as the
  // face rotates. Drawn BEFORE the front face so the front overlaps it.
  int rimW = (fx >= 0 ? fx : -fx);   // |fx|
  if (rimW > 0) {
    // Rim sits on the OPPOSITE side of the tilt direction.
    int rimX = (fx > 0) ? (x - rimW) : (x + HW);
    _t->fillRect(rimX, y + 4, rimW, HH - 8, CHASSIS_SH);
    // Rim edge highlight (thin line where rim meets front face)
    int edgeX = (fx > 0) ? x - 1 : x + HW;
    _t->drawFastVLine(edgeX, y + 4, HH - 8, 0x52AA);
  }

  // ── FRONT FACE ──  The chassis the user sees
  _t->fillRoundRect(x, y, HW, HH, 8, CHASSIS);
  _t->drawRoundRect(x, y, HW, HH, 8, CHASSIS_SH);

  // ── DIRECTIONAL LIGHT GRADIENT ──  Vertical bright strip on the side
  // facing the implied light source. Strength proportional to tilt.
  int litStrength = (fx >= 0 ? fx : -fx);
  if (litStrength > 0) {
    int litX = (fx > 0) ? x + HW - 5 : x + 2;
    int litW = (litStrength + 1) / 2;   // 1-4 px
    if (litW > 3) litW = 3;
    _t->fillRect(litX, y + 6, litW, HH - 12, CHASSIS_HI);
  }

  // ── TOP EDGE HIGHLIGHT ──  Moving light source on the forehead
  int hliteShift = _tiltX;
  int hStart = x + 6 + hliteShift;
  int hEnd   = x + HW - 6 + hliteShift;
  if (hStart < x + 4) hStart = x + 4;
  if (hEnd   > x + HW - 4) hEnd = x + HW - 4;
  if (hEnd > hStart) {
    _t->drawFastHLine(hStart, y + 1, hEnd - hStart, CHASSIS_HI);
  }

  // Top vent — three small horizontal slits (tracking face plane)
  for (int i = 0; i < 3; i++) {
    _t->drawFastHLine(HX + fx - 8 + i * 8 - 3, y + 5, 6, CHASSIS_SH);
  }

  // Side panel lines (suggests modular construction)
  _t->drawFastVLine(x + 6,        y + 14, HH - 22, CHASSIS_SH);
  _t->drawFastVLine(x + HW - 7,   y + 14, HH - 22, CHASSIS_SH);

  // Corner rivets (small dark dots)
  _t->fillCircle(x + 4,        y + 4,        1, CHASSIS_SH);
  _t->fillCircle(x + HW - 5,   y + 4,        1, CHASSIS_SH);
  _t->fillCircle(x + 4,        y + HH - 5,   1, CHASSIS_SH);
  _t->fillCircle(x + HW - 5,   y + HH - 5,   1, CHASSIS_SH);
}

// Visor — wide horizontal eye stripe (the bot's "real eyes").
// A bright "pupil" wedge inside the visor shifts with tilt for parallax —
// the bot literally looks where you tilt the device.
static void drawVisor(uint16_t color, int yOff) {
  int fx = faceOffX();
  int fy = faceOffY();
  int vY = HY - 6 + yOff + fy;
  int vX = HX - 18 + fx;
  int vW = 36;
  int vH = 10;
  // Recessed dark frame
  _t->fillRoundRect(vX - 1, vY - 1, vW + 2, vH + 2, 3, CHASSIS_SH);
  // Visor surface
  _t->fillRoundRect(vX, vY, vW, vH, 2, color);
  // Inner scanline (suggests CRT/LED grid)
  _t->drawFastHLine(vX + 2, vY + vH/2, vW - 4, CHASSIS_SH);
  // Tiny pixel grid for "screen" texture (every 4 px)
  for (int x = vX + 4; x < vX + vW - 2; x += 4) {
    _t->drawPixel(x, vY + 2, CHASSIS_SH);
    _t->drawPixel(x, vY + vH - 3, CHASSIS_SH);
  }
  // Parallax "pupil" — bright dot inside the visor. Sits at face center
  // PLUS an additional tilt amplification, so the pupil moves faster
  // than the head — reads as an eyeball tracking inside the head.
  uint16_t pupilColor = SPECULAR;
  int pupilX = HX + fx + _tiltX * 2;
  if (pupilX < vX + 3) pupilX = vX + 3;
  if (pupilX > vX + vW - 4) pupilX = vX + vW - 4;
  _t->fillRect(pupilX - 1, vY + 2, 3, vH - 4, pupilColor);
  _t->drawPixel(pupilX, vY + 1, color);  // small bright cap
  // Corner reflections
  _t->drawPixel(vX + 2, vY + 1, SPECULAR);
  _t->drawPixel(vX + vW - 3, vY + 1, SPECULAR);
}

// Sunglasses overlay — opaque black across the visor, with brand-style
// aviator frame and bridge. Lens highlights shift with tilt so the
// reflections appear to track the "light source" — a small but
// distinctly 3D-feeling detail.
static void drawSunglasses(int yOff) {
  int fx = faceOffX();
  int fy = faceOffY();
  int cx = HX + fx;                  // face-plane center
  int eyeY = HY - 1 + yOff + fy;
  // Two lens circles overlapping for a "wraparound aviator" silhouette
  _t->fillCircle(cx - 12, eyeY, 9, INK);
  _t->fillCircle(cx + 12, eyeY, 9, INK);
  // Frame outline (slightly thicker via double draw)
  _t->drawCircle(cx - 12, eyeY, 9, STEEL);
  _t->drawCircle(cx - 12, eyeY, 10, STEEL);
  _t->drawCircle(cx + 12, eyeY, 9, STEEL);
  _t->drawCircle(cx + 12, eyeY, 10, STEEL);
  // Nose bridge — bar between lenses
  _t->fillRect(cx - 4, eyeY - 1, 8, 3, STEEL);
  // Specular reflections — extra parallax (beyond face shift) so they
  // appear to track a fixed light source as the head rotates.
  int rx = _tiltX;
  _t->drawPixel(cx - 15 + rx, eyeY - 5, SPECULAR);
  _t->drawPixel(cx - 14 + rx, eyeY - 5, SPECULAR);
  _t->drawPixel(cx + 9  + rx, eyeY - 5, SPECULAR);
  _t->drawPixel(cx + 10 + rx, eyeY - 5, SPECULAR);
  // Reflection arcs follow the highlight position
  _t->drawLine(cx - 16 + rx, eyeY - 2, cx - 13 + rx, eyeY - 5, 0x4208);
  _t->drawLine(cx +  8 + rx, eyeY - 2, cx + 11 + rx, eyeY - 5, 0x4208);
}

// Speaker-grille mouth. mood 0=neutral, 1=closed flat, 2=smile arc,
// 3=grimace (zigzag), 4=open shout O, 5=tongue, 6=X dead
static void drawMouth(int mood, int yOff) {
  int mY = HY + 14 + yOff + faceOffY();
  int mX = HX + faceOffX();
  switch (mood) {
    case 0:  // neutral speaker grille
      for (int i = 0; i < 4; i++) {
        _t->drawFastHLine(mX - 8, mY - 3 + i * 2, 16, CHASSIS_SH);
      }
      break;
    case 1:  // closed thin
      _t->fillRect(mX - 7, mY, 14, 2, CHASSIS_SH);
      break;
    case 2:  // smile arc
      _t->drawLine(mX - 8, mY - 1, mX - 1, mY + 3, CHASSIS_SH);
      _t->drawLine(mX - 1, mY + 3, mX + 1, mY + 3, CHASSIS_SH);
      _t->drawLine(mX + 1, mY + 3, mX + 8, mY - 1, CHASSIS_SH);
      break;
    case 3:  // grimace (focused, jagged)
      for (int i = -8; i <= 6; i += 2) {
        int up = (i / 2) & 1 ? -1 : 1;
        _t->drawLine(mX + i, mY + up, mX + i + 2, mY - up, CHASSIS_SH);
      }
      break;
    case 4:  // open O (shouting/alarm)
      _t->fillCircle(mX, mY + 1, 4, INK);
      _t->drawCircle(mX, mY + 1, 4, CHASSIS_SH);
      _t->drawCircle(mX, mY + 1, 5, CHASSIS_SH);
      break;
    case 5:  // tongue zigzag
      _t->drawLine(mX - 6, mY, mX, mY + 3, 0xC000);
      _t->drawLine(mX, mY + 3, mX + 6, mY, 0xC000);
      break;
    case 6:  // X
      _t->drawLine(mX - 5, mY - 2, mX + 5, mY + 3, CHASSIS_SH);
      _t->drawLine(mX - 5, mY + 3, mX + 5, mY - 2, CHASSIS_SH);
      break;
  }
}

// Lightning bolt — gr0m brand logo, used as the chest status indicator.
// Color carries mood (idle cyan, busy green, alert red, etc.); placement
// is on the chest plate as a permanent identity marker.
static void drawBolt(int cx, int cy, uint16_t fill, uint16_t glow) {
  // Outer glow halo (slightly larger, dimmer)
  _t->fillTriangle(cx + 3, cy - 7, cx - 3, cy - 7, cx - 1, cy + 1, glow);
  _t->fillTriangle(cx + 3, cy - 7, cx - 1, cy + 1, cx + 3, cy + 1, glow);
  _t->fillTriangle(cx + 3, cy + 1, cx - 3, cy + 1, cx - 3, cy + 7, glow);
  // Bolt fill — classic 3-segment zigzag
  _t->fillTriangle(cx + 2, cy - 6, cx - 2, cy - 6, cx - 1, cy + 1, fill);
  _t->fillTriangle(cx + 2, cy - 6, cx - 1, cy + 1, cx + 2, cy + 1, fill);
  _t->fillTriangle(cx + 2, cy + 1, cx - 1, cy + 1, cx - 2, cy + 6, fill);
  // Highlight stroke along the leading edge
  _t->drawLine(cx + 2, cy - 6, cx - 1, cy + 1, SPECULAR);
}

// Neck + chest panel — features the lightning-bolt brand logo as
// the always-visible status indicator. Color of the bolt changes
// with mood (passed from each state function).
// Chest stays ANCHORED (no face offset) — the head pivots on the neck.
// Neck is drawn as a trapezoid connecting the shifted head bottom to
// the anchored chest top, so it flexes as the head rotates.
static void drawChest(uint16_t boltColor, int yOff) {
  int fx = faceOffX();
  int fy = faceOffY();
  int hbY = HY + HH/2 + yOff + fy;     // top of neck (under head)
  int cbY = HY + HH/2 + yOff + 6;      // bottom of neck (chest top)
  // Trapezoid neck — left & right triangles fill the slanted region
  _t->fillTriangle(HX + fx - 3, hbY,
                   HX + fx + 3, hbY,
                   HX + 3,      cbY, CHASSIS_SH);
  _t->fillTriangle(HX + fx - 3, hbY,
                   HX + 3,      cbY,
                   HX - 3,      cbY, CHASSIS_SH);
  // Highlight stripe along the left edge of the neck
  _t->drawLine(HX + fx - 3, hbY, HX - 3, cbY, CHASSIS_HI);

  // Chest plate
  int cX = HX - 22;
  int cY = HY + HH/2 + 7 + yOff;
  _t->fillRoundRect(cX, cY, 44, 18, 4, CHASSIS);
  _t->drawRoundRect(cX, cY, 44, 18, 4, CHASSIS_SH);
  // Top highlight
  _t->drawFastHLine(cX + 4, cY + 1, 36, CHASSIS_HI);
  // Side grille slits (suggesting venting around the chest core)
  for (int i = 0; i < 3; i++) {
    _t->drawFastVLine(cX + 5 + i * 3, cY + 5, 8, CHASSIS_SH);
    _t->drawFastVLine(cX + 39 - i * 3, cY + 5, 8, CHASSIS_SH);
  }
  // Lightning bolt centered on the chest — gr0m's brand mark
  // Glow color = dim version of fill (for visual depth)
  uint16_t glow = (boltColor >> 1) & 0x7BEF;  // halve brightness via bit shift
  drawBolt(HX, cY + 9, boltColor, glow);
}

// ── BUSY-only: discarded item tumbling at the upper-left corner ────

// Glasses tumbling in upper-left, cycles through 4 orientation poses
static void drawDiscardedGlasses(uint32_t t) {
  // Static base position with small arc bob
  static const int8_t BOB_X[4] = { 0, 2, 4, 2 };
  static const int8_t BOB_Y[4] = { 0, -2, 0, 2 };
  uint8_t pose = (t / 3) % 4;
  int x = pkX(16 + BOB_X[pose]);
  int y = pkY(18 + BOB_Y[pose]);
  // Draw glasses in one of 4 orientations
  switch (pose) {
    case 0:  // horizontal
      _t->fillCircle(x - 5, y, 4, INK);
      _t->fillCircle(x + 5, y, 4, INK);
      _t->drawLine(x - 1, y, x + 1, y, STEEL);
      _t->drawCircle(x - 5, y, 4, STEEL);
      _t->drawCircle(x + 5, y, 4, STEEL);
      break;
    case 1:  // tilted up-right
      _t->fillCircle(x - 4, y + 2, 4, INK);
      _t->fillCircle(x + 4, y - 2, 4, INK);
      _t->drawLine(x - 1, y + 1, x + 1, y - 1, STEEL);
      _t->drawCircle(x - 4, y + 2, 4, STEEL);
      _t->drawCircle(x + 4, y - 2, 4, STEEL);
      break;
    case 2:  // vertical (flipped)
      _t->fillCircle(x, y - 5, 4, INK);
      _t->fillCircle(x, y + 5, 4, INK);
      _t->drawLine(x, y - 1, x, y + 1, STEEL);
      _t->drawCircle(x, y - 5, 4, STEEL);
      _t->drawCircle(x, y + 5, 4, STEEL);
      break;
    case 3:  // tilted down-right
      _t->fillCircle(x - 4, y - 2, 4, INK);
      _t->fillCircle(x + 4, y + 2, 4, INK);
      _t->drawLine(x - 1, y - 1, x + 1, y + 1, STEEL);
      _t->drawCircle(x - 4, y - 2, 4, STEEL);
      _t->drawCircle(x + 4, y + 2, 4, STEEL);
      break;
  }
  // Motion lines suggesting toss
  uint16_t blur = 0x4208;
  _t->drawPixel(x - 10, y, blur);
  _t->drawPixel(x - 12, y + 1, blur);
  _t->drawPixel(x - 14, y, blur);
}

// ── Mood particles ──────────────────────────────────────────────────
// Hearts rise on the left, fading into small neutral sparkles at the
// half-life of their flight.
static void drawMoodParticles(uint32_t t, int n, int speed) {
  if (evoStage() < 4) return;            // Stage 4 (HUD): mood particles
  const int LIFECYCLE = 28;
  const int FADE = (LIFECYCLE * 2) / 3;   // heart for the lower ~2/3 of the rise;
                                          // only a brief soft twinkle near the top
  int baseX = -32;
  for (int i = 0; i < n; i++) {
    int phase = ((int)t * 2 / speed + i * 5) % LIFECYCLE;
    int y = HY + 38 - phase;
    if (y < 2 || y > HY + 40) continue;
    int x = HX + baseX + (n > 1 ? (i * 22) / (n - 1) : 0);
    // Drift with gravity only — the old ±1-per-step x-jitter made the motes
    // shimmer/jump which, with the stark-white sparkle, read as a flicker.
    x += (-_gravX * phase) / 8;
    if (phase < FADE) {
      drawHeart(pkX(x), pkY(y), HEART_RED);
    } else {
      // Fade-out terminus: a soft PINK-WHITE twinkle (not stark white) so the
      // heart→mote handoff is a gentle fade beside the bot, not a pink↔white blink.
      drawSparkle(pkX(x), pkY(y), MOTE_TWINKLE);
    }
  }
}

// ── Enrichments — extra gear and decorations layered onto the base bot ─

// Headphones — arched band over the head with two ear-cups + red foam.
// Drawn AFTER the head so it overlays. Tracks face-plane offset so it
// rides with head rotation.
static void drawHeadphones() {
  if (buddyScale() == 1) return;
  int fx = faceOffX();
  int fy = faceOffY();
  int cx = HX + fx;
  // Canvas: band arc `M 44 38 Q 67 24 90 38` — endpoints hug the forehead
  // (head top is y=32) and the arch crests ~7 px above them at center. The
  // SVG x/y units map 1:1 to device pixels (same HX/HY anchor), so the band
  // sweeps cx±23 between y=38 (ends) and y≈31 (apex). 2-px steel stroke with
  // a lighter inner highlight, matching the two stacked <path> strokes.
  //
  // The headphones are authored in raw HOME screen coords (they don't go
  // through projectV like the face decorations do), so when the DJ scene
  // shrinks + raises the head we run every point through the SAME head
  // transform (djHeadMapX/Y + _djHeadScale) so the cans stay glued to the
  // smaller head. Outside the DJ scene the transform is the identity, so
  // this renders byte-for-byte as before.
  const float s = _djHeadScale;
  int endY = HY - HH/2 + fy + 6;     // y=38 — just below the head-top edge
  for (int i = -23; i <= 23; i++) {
    float n = (float)i / 23.0f;
    int yOff = (int)((1.0f - n * n) * 7.0f);   // crest 7 px over the ends
    int x = djHeadMapX(cx + i);
    int y = djHeadMapY(endY - yOff);
    _t->drawPixel(x, y,     STEEL);
    _t->drawPixel(x, y + 1, STEEL);
    _t->drawPixel(x, y,     0x52AA);            // inner highlight stroke
  }
  // Ear cups — sit over the head sides at y=44 (canvas x=38 / x=90, 6×10).
  // Fixed (don't rotate with face) so they read like real over-ears. Cup
  // dimensions scale with the head so they don't dwarf a shrunk DJ head.
  int eY = HY - HH/2 + fy + 12;      // y=44
  int cupW = (int)lroundf(6 * s); if (cupW < 2) cupW = 2;
  int cupH = (int)lroundf(10 * s); if (cupH < 3) cupH = 3;
  int lX = djHeadMapX(cx - 29), rX = djHeadMapX(cx + 23);
  int cY = djHeadMapY(eY);
  _t->fillRect(lX, cY, cupW, cupH, STEEL);       // left cup  (x=38)
  _t->fillRect(rX, cY, cupW, cupH, STEEL);       // right cup (x=90)
  // Cup shadow seam on the left can (canvas Px 38,45,1,8)
  _t->fillRect(lX, cY + 1, 1, cupH - 2 < 1 ? 1 : cupH - 2, 0x52AA);
  // Red foam ring stripe (canvas: x=43 left / x=90 right, y=47, 1×4)
  int foamH = (int)lroundf(4 * s); if (foamH < 1) foamH = 1;
  _t->fillRect(djHeadMapX(cx - 24), cY + (cupH * 3 / 10), 1, foamH, CRIMSON);
  _t->fillRect(djHeadMapX(cx + 23), cY + (cupH * 3 / 10), 1, foamH, CRIMSON);
  // Subtle highlight specks on the outer cup corners
  _t->drawPixel(lX, cY, SPECULAR);
  _t->drawPixel(djHeadMapX(cx + 28), cY, SPECULAR);
}

// Small hands at the chest area — used when typing.
// Two stubby grey circles either side of the laptop, with thin arms back
// to the chest body.
static void drawHandsAtLaptop() {
  // Canvas Hands(pose='type'): two stubby hands resting on the keyboard at
  // y≈111 (just above the kb top y=114), flanking it at x≈52 / x≈81 — i.e.
  // cx±15. Short chassis-shadow arm stubs reach back up toward the chest,
  // each capped by a chassis-color knuckle circle (r=3) with a 1-px highlight.
  int cx = pkX(HX);
  int hy = pkY(111);                 // hand-circle center (canvas y+1 = 111)
  int ay = pkY(105);                 // arm stub top — bridges up to the body
  int hr = pkS(3); if (hr < 1) hr = 1;
  // Arms — diagonal chassis-shadow stubs from the chest down onto the keys.
  // (The canvas draws short horizontal 6×4 stubs; the firmware angles them so
  //  they read as forearms tucking under the head, matching the 3D body.)
  _t->drawLine(cx - pkS(19), ay, cx - pkS(15), hy - pkS(2), CHASSIS_SH);
  _t->drawLine(cx - pkS(18), ay, cx - pkS(14), hy - pkS(2), CHASSIS_SH);
  _t->drawLine(cx + pkS(19), ay, cx + pkS(15), hy - pkS(2), CHASSIS_SH);
  _t->drawLine(cx + pkS(18), ay, cx + pkS(14), hy - pkS(2), CHASSIS_SH);
  // Knuckles (canvas circles at x=52 / x=81 → cx∓15)
  _t->fillCircle(cx - pkS(15), hy, hr, CHASSIS);
  _t->drawCircle(cx - pkS(15), hy, hr, CHASSIS_SH);
  _t->drawPixel(cx - pkS(16), hy - 1, CHASSIS_HI);
  _t->fillCircle(cx + pkS(15), hy, hr, CHASSIS);
  _t->drawCircle(cx + pkS(15), hy, hr, CHASSIS_SH);
  _t->drawPixel(cx + pkS(14), hy - 1, CHASSIS_HI);
}

// Tiny laptop sat in front of gr0m, glowing green screen with fake code.
// Drawn between chest and bottom edge of screen so it reads as "on lap".
static void drawLaptop(uint32_t t) {
  // Canvas geometry is in absolute screen units (keyboard y=114, screen
  // y=102) — it sits low, "on the lap" near the screen bottom, NOT up at the
  // chest. Anchor to those coords and run them through pk* so the peek card
  // shrink still works. cx=67 matches the canvas x-center (laptop spans
  // x=48..86, center ≈67).
  int cx = pkX(HX);
  int kx = cx - pkS(19), ky = pkY(114);      // keyboard base (canvas 48,114)
  int kw = pkS(38), kh = pkS(3); if (kh < 1) kh = 1;
  // Keyboard base + lip shadow
  _t->fillRect(kx, ky, kw, kh, 0x39E8);                       // #3a3d40
  _t->drawFastHLine(kx - 1, ky + kh, kw + 2, 0x18E4);         // #1a1d20 lip
  // Key hint dots (8 keys at x=50+i*4.5)
  for (int i = 0; i < 8; i++)
    _t->drawPixel(cx - pkS(17) + pkS((i * 9) / 2), ky + 1, 0x7BF0);  // #7a7d80
  // Hinge strip just above the keyboard
  _t->drawFastHLine(cx - pkS(17), ky - 1, pkS(34), 0x5AEC);   // #5a5d60
  // Screen bezel + glowing panel (canvas bezel 51,102,32,12; screen 52,103,30,10)
  int sx = cx - pkS(16), sy = pkY(102);
  int sw = pkS(32), sh = pkS(12); if (sh < 1) sh = 1;
  _t->fillRect(sx, sy, sw, sh, 0x18E4);                       // #1a1d20 bezel
  _t->fillRect(sx + 1, sy + 1, sw - 2, sh - 2, 0x0C47);       // #0f8a3a screen
  // Fake code lines — DIM green glyphs over the brighter screen (canvas
  // #1a4a22, deliberately darker than the panel). Coords relative to bezel:
  // canvas absolute (54,105)→bezel+(3,3), etc.
  _t->drawFastHLine(sx + pkS(3),  sy + pkS(3), pkS(6),  0x1A44);
  _t->drawFastHLine(sx + pkS(11), sy + pkS(3), pkS(4),  0x1A44);
  _t->drawFastHLine(sx + pkS(3),  sy + pkS(5), pkS(3),  0x1A44);
  _t->drawFastHLine(sx + pkS(8),  sy + pkS(5), pkS(8),  0x1A44);
  _t->drawFastHLine(sx + pkS(3),  sy + pkS(7), pkS(10), 0x1A44);
  _t->drawFastHLine(sx + pkS(15), sy + pkS(7), pkS(2),  0x1A44);
  // Blinking cursor — bright mint (canvas #a4ffb4 at 70,111 → bezel+(19,9))
  if ((t / 2) & 1) _t->fillRect(sx + pkS(19), sy + pkS(9), pkS(2), 1, 0xA7F6);
}

// Speech bubble floating above gr0m's head. White rounded rect with black
// outline and a tail that always points back down at the head. Text rendered
// in `textColor`. The bubble is CENTERED over the head and clamped to stay
// fully on the 135px screen (and below the top edge), and its tail tracks the
// head center so it reads as the bot's own speech even when the body is the
// shrunk peek figure.
static void drawSpeechBubble(const char* text, uint16_t textColor) {
  int len = 0; while (text[len]) len++;
  int w = len * 6 + 6; if (w < 20) w = 20;   // canvas min width
  int h = 12;
  int headX = pkX(HX);                 // who's talking — head center
  // Canvas places the bubble up and to the RIGHT of the head (jsx x=95;
  // SPEC `x = HX + 30`) with the tail angling back-left to the head. Offset
  // it to the upper-right, then clamp horizontally so it stays on the 135-px
  // screen.
  int x = headX + 30;
  if (x + w > 134) x = 134 - w;
  if (x < 1) x = 1;
  // Sit the bubble above the head, but never let it (or its tail anchor) clip
  // off the top — leave room for the 4px tail beneath the body.
  int y = pkY(HY - HH / 2) - h - 6;
  if (y < 1) y = 1;
  // body
  _t->fillRoundRect(x, y, w, h, 2, SPECULAR);
  _t->drawRoundRect(x, y, w, h, 2, INK);
  // Tail — a small wedge under the body that points at the head center,
  // clamped to stay attached to the bubble so it never detaches/floats.
  int tipX = headX;
  if (tipX < x + 3) tipX = x + 3;
  if (tipX > x + w - 3) tipX = x + w - 3;
  int baseL = tipX - 3, baseR = tipX + 3;
  if (baseL < x + 1) baseL = x + 1;
  if (baseR > x + w - 1) baseR = x + w - 1;
  _t->fillTriangle(baseL, y + h - 1, baseR, y + h - 1, tipX, y + h + 4, SPECULAR);
  _t->drawLine(baseL, y + h - 1, tipX, y + h + 4, INK);
  _t->drawLine(baseR, y + h - 1, tipX, y + h + 4, INK);
  // text
  _t->setTextSize(1);
  _t->setTextColor(textColor, SPECULAR);
  _t->setCursor(x + 3, y + 3);
  _t->print(text);
}

// Party hat — pointy triangle, stripes, pom. #tilt-gravity FALLING → slide
// down-side + apex lean (faked rotate).
static void drawPartyHat() {
  int fx = faceOffX();
  int fy = faceOffY();
  int hx = pkX(HX + fx);
  int hy = pkY(HY - HH/2 + fy);       // head top = canvas y=32 = hat base
  int slideX = 0, slideY = 0, apexLean = 0;   // #tilt-gravity detach offsets
  if (_hatState == ITEM_FALLING) {
    float a = fabsf(_tiltDegX) - 35.0f; if (a > 30.0f) a = 30.0f;
    int s = (int)(a * (_tiltDegX > 0 ? 1 : -1));
    slideX = pkS(s * 3 / 2);
    slideY = -pkS(abs(s) * 2 / 5);             // pops up
    apexLean = pkS((int)(_tiltDegX * 0.3f));   // tip leans
    if (apexLean >  pkS(24)) apexLean =  pkS(24);
    if (apexLean < -pkS(24)) apexLean = -pkS(24);
  }
  hx += slideX; hy += slideY;
  // Canvas: triangle 67,12 / 56,32 / 78,32 — base sits ON the head top, apex
  // 20 px up, half-width 11. Crimson fill, dark outline.
  int up = pkS(20), out = pkS(11);
  int ax = hx + apexLean;
  _t->fillTriangle(ax, hy - up, hx - out, hy, hx + out, hy, CRIMSON);
  _t->drawTriangle(ax, hy - up, hx - out, hy, hx + out, hy, INK);
  // White stripes (canvas y=22 / y=28 → 10 / 4 px above the base)
  _t->drawLine(hx - pkS(5) + apexLean/2, hy - pkS(10), hx + pkS(6) + apexLean/2, hy - pkS(10), SPECULAR);
  _t->drawLine(hx - pkS(8) + apexLean/3, hy - pkS(4),  hx + pkS(9) + apexLean/3, hy - pkS(4),  SPECULAR);
  // Yellow pom rides the leaned tip (canvas circle 67,11 r=3 #ffd60a)
  int pr = pkS(3); if (pr < 1) pr = 1;
  _t->fillCircle(ax, hy - pkS(21), pr, 0xFEA1);   // #ffd60a
  _t->drawPixel(ax - 1, hy - pkS(22), SPECULAR);
}

// Nightcap — drooping cap that hangs to the right, white trim band, pom.
static void drawNightcap() {
  int fx = faceOffX();
  int fy = faceOffY();
  int hx = pkX(HX + fx);
  int hy = pkY(HY - HH/2 + fy);
  int half = pkS(24), bandH = pkS(3); if (bandH < 1) bandH = 1;
  // White trim band wrapping the head top (canvas Px 42,34,48,3 — sits on
  // the forehead just below the head top at y=32).
  _t->fillRect(hx - half, hy + 1, half * 2, bandH, SPECULAR);
  _t->drawFastHLine(hx - half, hy + 1, half * 2, 0xC638);
  // Drooping body — a magenta cap that humps up near center-left and slumps
  // down to the right where the pom hangs. The canvas draws this as a bezier
  // (M42,36 Q55,14 88,36 …); here it's approximated by a stack of HLines that
  // climb up and to the right — see report note on the curve simplification.
  int segs = (buddyScale() == 1) ? 9 : 18;
  for (int i = 0; i < segs; i++) {
    int y = hy - pkS(3) - i;
    int xLeft  = hx - pkS(18) + pkS(i * 2);
    int xRight = hx - pkS(8)  + pkS(i * 2);
    _t->drawFastHLine(xLeft, y, xRight - xLeft, VISOR_LOVE);
  }
  // Pom-pom on the slumped tip — sways toward the down-side with device tilt
  // (canvas: pomX=88+clamp(tilt*0.4,±12), pomY=20+|tilt|*0.2). Uses the
  // degree tilt (_tiltDegX) for consistency with the rest of the tilt system
  // (pupil tracking, gravity detach); scaled so the on-screen sway range
  // stays ±8-ish like the old integer _tiltX path.
  int sway = _clamp(_tiltDegX * 0.09f, -12, 12);
  int px = hx + pkS(18) + pkS(sway), py = hy - pkS(22) + pkS((int)(fabsf(_tiltDegX) / 56.0f));
  int pr = pkS(3); if (pr < 1) pr = 1;
  // Short magenta stalk from the cap tip out to the pom (canvas line, w4)
  _t->drawLine(hx + pkS(8), hy - pkS(20), px, py, VISOR_LOVE);
  _t->fillCircle(px, py, pr, SPECULAR);
  _t->drawPixel(px - 1, py - 1, 0xC638);
}

// Z particles drifting up — used in sleep state.
static void drawZParticles(uint32_t t) {
  bool peek = (buddyScale() == 1);
  _t->setTextColor(SPECULAR, BUDDY_BG);
  _t->setTextSize(1);
  int p1 = (int)(t * 2) % 30;
  if (p1 < 26) {
    _t->setCursor(pkX(HX + 12), pkY(HY - 16 - p1));
    _t->print("z");
  }
  int p2 = (int)((t + 5) * 2) % 30;
  if (p2 < 26) {
    if (!peek) _t->setTextSize(2);   // big "Z" stays size 1 in the mini strip
    _t->setCursor(pkX(HX + 18), pkY(HY - 24 - p2));
    _t->print("Z");
    _t->setTextSize(1);
  }
}

// Confetti rain — for celebrate.
static void drawConfetti(uint32_t t) {
  // In peek the confetti still rains across the full 135-px width but is
  // clamped to the upper strip (above the y=70 stats panel) with fewer
  // pieces so it reads as celebration without crowding the card.
  bool peek = (buddyScale() == 1);
  int n = peek ? 7 : 14;
  int maxY = peek ? 64 : 132;
  // Canvas club palette: #ff2d92, #ffd60a, #00bdff, #29d65b, #ff8c1a.
  static const uint16_t CONF_COL[] = { HEART_RED, 0xFFE0, VISOR_IDLE, VISOR_BUSY, 0xFC63 };
  for (int i = 0; i < n; i++) {
    int phase = ((int)t * 2 + i * 11) % 40;
    int x = (i * 11 + (int)t * 3) % 135;
    int y = (phase * 4) % (maxY + 1);
    if (y < 0 || y > maxY) continue;
    uint16_t c = CONF_COL[i % 5];
    if (i & 1) _t->fillRect(x, y, 2, 3, c);
    else       _t->fillCircle(x, y, 1, c);
  }
}

// Extra heart cloud — sparser but with bigger hearts. Heart state only.
static void drawHeartCloud(uint32_t t) {
  static const int8_t HPOS[][2] = {
    {18, 50}, {28, 28}, {10, 92}, {110, 38}, {118, 72}, {102, 100}
  };
  for (int i = 0; i < 6; i++) {
    int phase = ((int)t * 2 + i * 7) % 24;
    int y = HPOS[i][1] - phase;
    int x = HPOS[i][0] + ((i & 1) ? 1 : -1);
    if (y < 0) continue;
    drawHeart(pkX(x), pkY(y), HEART_RED);
  }
}

// ── Ride mode (#tilt-skate / #tilt-surf / #tilt-hover) ───────────────
// Boards UNDER the bot (y≈132-150), geometry from gr0m.jsx, pivot (67,138).
// Vertical-SHEAR about x=67 (y'=y+(x-67)*slope; affine rotate too costly).
// Home-scale only. 0 none/1 skate/2 surf/3 hover.
//
// AUTOMATIC at Stage 5: the rides are no longer a user toggle. Like the DJ
// idle-flourish, drawRide() drops in a board on a periodic idle window once the
// character has fully evolved, cycling skate → surf → hover across successive
// windows so all three appear over time. Below Stage 5 no board ever shows.
static const int RIDE_PIVOT_X = 67;
static int _rideSlope = 0;     // y shear = (x-67)*_rideSlope>>8

// slope ≈ angle(rad)×256. factor = JSX rotate mult (skate .6, surf/hover .5).
static inline void rideComputeSlope(float f) {
  _rideSlope = (int)(_tiltDegX * f * 0.0174533f * 256.0f);
}
static inline int rideY(int x, int y) { return y + ((x - RIDE_PIVOT_X) * _rideSlope >> 8); }

// #tilt-skate — brown deck, orange bolt, trucks, r3 wheels, shadow.
static void drawSkateboard() {
  rideComputeSlope(0.6f);
  _t->fillEllipse(RIDE_PIVOT_X + (_rideSlope >> 4), 148, 32, 2, 0x10A2);
  for (int i = 0; i < 70; i++) {              // sheared brown deck
    int x = 32 + i, yt = rideY(x, 134);
    _t->drawFastVLine(x, yt, 6, 0x59C4);
    _t->drawPixel(x, yt, 0x7AC7);
    _t->drawPixel(x, yt + 5, 0x18A1);
  }
  int by = rideY(63, 137);
  _t->fillTriangle(64, by - 1, 61, by, 65, by, 0xFC63);
  _t->fillTriangle(64, by + 1, 61, by, 65, by, 0xFC63);
  _t->fillRect(40, rideY(43, 140), 6, 2, 0x8410);
  _t->fillRect(88, rideY(91, 140), 6, 2, 0x8410);
  for (int k = 0; k < 2; k++) {
    int wx = k ? 92 : 42, wy = rideY(wx, 144);
    _t->fillCircle(wx, wy, 3, 0xDEDB);
    _t->drawCircle(wx, wy, 3, 0x2965);
    _t->drawPixel(wx, wy, 0x8410);
  }
}

// #tilt-surf — wave curl + pointy board + red stripe + fin.
static void drawSurfboard() {
  rideComputeSlope(0.5f);
  for (int i = 0; i < 106; i++) {
    int x = 14 + i, hump = 6 - (abs(x - 67) * 6) / 53;
    int yt = rideY(x, 144 - hump);
    _t->drawFastVLine(x, yt, 152 - yt, 0x1B59);
    _t->drawPixel(x, yt, 0x5DBF);
  }
  _t->fillRect(32, rideY(34, 134), 4, 1, SPECULAR);
  _t->fillRect(98, rideY(100, 134), 4, 1, SPECULAR);
  int n = rideY(67, 134), e = rideY(106, 140), w = rideY(28, 140), s = rideY(67, 142);
  _t->fillTriangle(28, w, 67, n, 106, e, SPECULAR);
  _t->fillTriangle(28, w, 106, e, 67, s, SPECULAR);
  _t->drawTriangle(28, w, 67, n, 106, e, 0x8410);
  _t->drawLine(36, rideY(36, 140), 100, rideY(100, 140), 0xF8A1);
  int fy = rideY(67, 142);
  _t->fillTriangle(62, fy, 67, fy + 6, 72, fy, 0x8410);
}

// #tilt-hover (Stage-5 alt) — MINIMAL STUB for the cap (see report).
static void drawHoverboard() {
  rideComputeSlope(0.5f);
  _t->fillEllipse(RIDE_PIVOT_X + (_rideSlope >> 4), 148, 28, 3, 0x32BF);
  int lx = 36, rx = 98, ly = rideY(lx, 135), ry = rideY(rx, 135);
  _t->fillTriangle(lx, ly, rx, ry, rx, ry + 4, 0x10A2);
  _t->fillTriangle(lx, ly, lx, ly + 4, rx, ry + 4, 0x10A2);
  _t->drawLine(lx, ly, rx, ry, VISOR_IDLE);
  _t->fillEllipse(48, rideY(48, 142), 3, 1, VISOR_IDLE);
  _t->fillEllipse(86, rideY(86, 142), 3, 1, VISOR_IDLE);
}

// Automatic Stage-5 idle flourish — a board drops in for a window once a
// minute, mirroring the DJ flourish cadence (60 s period, 8 s window). Across
// successive windows the board cycles skate → surf → hover so all three show
// over time. `t` is millis(). Home-scale only; gated to Stage 5 (Ascended) so
// the rides arrive as an automatic leveling upgrade, not a user toggle.
// Showcase override: when non-zero the cycle forces a specific board every
// frame (1 skate / 2 surf / 3 hover), bypassing the Stage-5 gate + idle timing.
static uint8_t _forceRide = 0;
// Showcase override: when true, doIdle draws the green digital LCD chest readout
// instead of the lightning bolt (the "digital costume" outfit's signature).
static bool _forceChestLcd = false;
static void drawRide(uint32_t t) {   // home-scale only
  if (buddyScale() != 2) return;
  if (_forceRide) {
    if      (_forceRide == 1) drawSkateboard();
    else if (_forceRide == 2) drawSurfboard();
    else                      drawHoverboard();
    return;
  }
  if (evoStage() < 5) return;            // Stage 5 (Ascended): rides unlock
  const uint32_t RIDE_PERIOD_MS = 60000; // idle cadence: a drop-in once a minute
  const uint32_t RIDE_WINDOW_MS = 8000;  // and it lasts ~8 s
  if ((t % RIDE_PERIOD_MS) >= RIDE_WINDOW_MS) return;
  // Cycle skate → surf → hover across successive windows.
  uint8_t which = (t / RIDE_PERIOD_MS) % 3;
  if      (which == 0) drawSkateboard();
  else if (which == 1) drawSurfboard();
  else                 drawHoverboard();
}

// Optional chest LCD — replaces the bolt with a small green readout.
// Opt-in via settings (e.g. settings.chestLcd = true). text is ≤4 chars
// to fit in the 20×11 inset.
static void drawChestLCD(const char* text) {
  if (evoStage() < 4) return;            // Stage 4 (HUD): chest readout
  if (!frontFaceVisible()) return;       // hide as the chest turns away (like the bolt)
  bool peek = (buddyScale() == 1);
  // Project onto the chest's front plane (z = +12, same plane as drawBolt3D)
  // so the readout ROTATES and scales WITH the body instead of floating as a
  // flat 2D overlay. projectV() handles the peek shrink — no pk* mapping.
  V2 tl = rp({ -15, 31, 12 });
  V2 tr = rp({  15, 31, 12 });
  V2 br = rp({  15, 41, 12 });
  V2 bl = rp({ -15, 41, 12 });
  // Lit green screen (two triangles) + frame outline.
  _t->fillTriangle(tl.x, tl.y, tr.x, tr.y, br.x, br.y, 0x02E0);
  _t->fillTriangle(tl.x, tl.y, br.x, br.y, bl.x, bl.y, 0x02E0);
  _t->drawLine(tl.x, tl.y, tr.x, tr.y, 0x0660);
  _t->drawLine(tr.x, tr.y, br.x, br.y, 0x0660);
  _t->drawLine(br.x, br.y, bl.x, bl.y, 0x0660);
  _t->drawLine(bl.x, bl.y, tl.x, tl.y, 0x0660);
  if (peek) return;                      // screen only — token text won't fit the mini
  // Token text centered on the projected screen, upright (like the DJ BPM tag).
  int textLen = 0; while (text[textLen]) textLen++;
  int cx = (tl.x + tr.x + bl.x + br.x) / 4;
  int cy = (tl.y + tr.y + bl.y + br.y) / 4;
  _t->setTextSize(1);
  _t->setTextColor(0x5FE0, 0x02E0);
  _t->setCursor(cx - textLen * 3, cy - 3);
  _t->print(text);
}

// Tiny pixel desk + coffee mug + steam, sat under gr0m as a screen-bottom
// band. Canvas #desk: desk top at y=132-134, mug at y=126-132, steam rising
// to y=120. The SVG units map 1:1 to device pixels, so the canvas coords are
// used almost verbatim — only the desk-top width is trimmed (x20→w90, ending
// at x=110) so it fits the 110-px-wide landscape pet sprite.
//
// LANDSCAPE-CLOCK ONLY: in the canvas the desk pairs with the landscape clock
// screen. That mode is the only one that renders the bot to a surface other
// than the main `spr` (it draws to petSpr / M5.Lcd via buddyRenderTo), so we
// gate on that. Portrait home, peek cards, and the DJ/costume scenes (all on
// `spr`) are unaffected — no desk appears there.
static void drawDesk() {
  if (buddyTarget() == &spr) return;     // landscape clock surfaces only
  if (buddyScale() != 1) return;         // landscape clock always renders at 1×
  // Desk top — two stacked bands (canvas Px 20,132,94,2 + 20,134,94,1).
  // Trimmed to w=90 so the right edge lands at x=110, the pet-sprite width.
  _t->fillRect(20, 132, 90, 2, 0x6A45);  // #6b4a2a desk surface
  _t->drawFastHLine(20, 134, 90, 0x3943);// #3a2818 front-edge shadow
  // Coffee mug (canvas 28,126 8×6) — white body, dark rims top + bottom.
  _t->fillRect(28, 126, 8, 6, 0xFFFF);
  _t->drawFastHLine(28, 126, 8, 0x2104); // #222 rim
  _t->drawFastHLine(28, 131, 8, 0x2104); // #222 base
  // Handle (canvas 36,127 2×3 white + 37,127 1×3 dark)
  _t->fillRect(36, 127, 2, 3, 0xFFFF);
  _t->drawFastVLine(37, 127, 3, 0x2104);
  // Coffee surface (canvas 29,127 6×1 #3a1808)
  _t->drawFastHLine(29, 127, 6, 0x38C1);
  // Steam wisps rising off the mug (canvas faint #dadddf dotted lines). 8-bit
  // sprites can't alpha-blend, so the fainter wisps use a dimmer grey instead
  // of opacity — see report note.
  _t->fillRect(30, 122, 1, 2, 0xDEFB);   // near wisp (op 0.7)
  _t->fillRect(32, 120, 1, 2, 0x9CD3);   // far wisp  (op 0.5 → dimmer grey)
  _t->drawPixel(34, 123, 0xDEFB);        // small puff
}

// ════════════════════════════════════════════════════════════════════
//   STAGE-5 FINAL FORM — the HUMAN COSTUME ("AGI cosplaying a human")
// ────────────────────────────────────────────────────────────────────
//   At the final evolution stage gr0m tries to pass as a person — and
//   every detail is slightly WRONG by design. Reconciled exactly to the
//   authoritative canvas in deployment/gr0m/SPEC.md #human-costume +
//   deployment/source/gr0m.jsx (HumanCostume). Six detail elements, drawn
//   BACK TO FRONT: hoodie → face mask → wig → antenna-poke → coffee →
//   bubble (see drawHumanCostumeFull). The primitives are authored in the
//   canvas's absolute pixel coords (face center 67,55) and mapped onto the
//   3D pipeline through cvFace()/cvChest(), so the whole disguise tracks
//   head/body tilt like the visor + brand bolt do.
// ════════════════════════════════════════════════════════════════════

// Project a point on the CHEST front-face plane (z = +12) to screen. Mirror
// of onFace() but for the body, so the hoodie + coffee track the chest the
// way the brand bolt does in drawBolt3D.
static V2 onChest(float x, float y) {
  return rp({ x, y, 12.0f });
}

// One-call composite for the Stage-5 disguise everyday look. RECONCILED to
// deployment/gr0m/SPEC.md #human-costume — the "AGI unconvincingly cosplaying
// a human" gag. Forward-declared here; the canvas-faithful primitives +
// drawHumanCostumeFull() composite live further down (after the onChest
// helper they depend on), so this just trampolines to it. Keeps the
// evoStage>=5 auto-render gating its callers already apply.
static void drawHumanCostumeFull(uint32_t t, bool showBubble = true);
// Thin alias kept for the A+B easter-egg path / external callers. The mood
// states deliberately do NOT call this — the costume is a separate state, not
// an overlay on the normal robot moods.
[[maybe_unused]] static void drawHumanDisguise(uint32_t t) {
  drawHumanCostumeFull(t);
}

// ════════════════════════════════════════════════════════════════════
//   HUMAN COSTUME PRIMITIVES — canvas-faithful Stage-5 disguise
// ────────────────────────────────────────────────────────────────────
//   The six detail elements of deployment/gr0m/SPEC.md #human-costume,
//   each one walked straight off the SVG in deployment/source/gr0m.jsx:
//     • #human-wig       — brown wig sitting ASKEW (-3,+2, ~-4°) + cowlicks
//     • #antenna-poke    — antenna piercing UP THROUGH the wig + LED halo
//     • #human-face-mask — flesh mask, CYAN visor leak below the edge,
//                          mismatched googly eyes, crooked marker brows,
//                          painted-on red rictus + misaligned seam, and a
//                          beard PEELING off the left with two falling chunks
//     • #human-hoodie    — navy hoodie, "100% HUMAN" name badge, $19.99 RED
//                          price tag dangling (-15°), drawstrings, pocket
//     • #coffee-cup      — "FELLOW HUMAN BEAN" sleeve + rising steam
//     • #human-bubble    — "BEEP BOOP FELLOW HUMAN" with a tail at the head
//
//   The head elements (wig/mask/antenna-poke) ride the HEAD front plane via
//   cvFace() and only render front-on, so they vanish as the head turns
//   away just like the visor. The hoodie + coffee ride the CHEST front
//   plane via cvChest() so they lean with the body. The bubble is a HUD
//   overlay drawn in absolute screen space.
//
//   Two entry points share these primitives:
//     • drawHumanDisguise()/drawHumanCostumeFull() — the evoStage>=5
//       auto-render final form (mood states call it when stage>=5)
//     • gr0mRenderHumanCostume() — the A+B manual full-screen toggle,
//       a standalone scene like the DJ booth
// ════════════════════════════════════════════════════════════════════

// Costume palette (RGB565) — reconciled exactly to deployment/gr0m/SPEC.md
// #human-costume + deployment/source/gr0m.jsx HumanCostume defaults. Each
// constant carries the source hex so the canvas remains the authority.
//
// The seven colors below are the ACTIVE costume palette: they default to the
// "undercover" (brown/navy) variant but are SWAPPED per variant by
// applyCostumeVariant() before each render, so the 50+ call sites below stay
// variant-agnostic. The "Pretending to Pay Taxes" variant (blond/maroon) is
// app.jsx gr-human-2's costumeOptions. Everything after HC_STRING is shared.
static uint16_t HC_SKIN     = 0xED90;  // #e8b386 flesh
static uint16_t HC_SKIN_SH  = 0xBC0B;  // #b8825a cheek / nose / mask-edge shadow
static uint16_t HC_HAIR     = 0x3923;  // #3a2418 brown wig + beard chunks
static uint16_t HC_HOODIE   = 0x3A4D;  // #3a4a6b navy knit hoodie
static uint16_t HC_HOODIE_DK= 0x29CB;  // #2a3858 pocket pouch
static uint16_t HC_HOODIE_LI= 0x5B51;  // #5a6a8b collar seam highlight
static uint16_t HC_HOODIE_TR= 0x1907;  // #1a2238 trim / hem
static const uint16_t HC_STRING   = 0xCE79;  // #cccccc drawstring cord
static const uint16_t HC_MOUTH    = 0xC9C7;  // #c83a3a painted-on red rictus smile
static const uint16_t HC_SEAM     = 0x79C3;  // #7a3818 faint misaligned mouth seam + cup rim
static const uint16_t HC_EYEBROW  = 0x1840;  // #1a0a04 marker eyebrows
static const uint16_t HC_CUP       = 0xFFFF; // #ffffff takeaway cup body
static const uint16_t HC_CUP_TOP   = 0x3923; // #3a2418 brown lid
static const uint16_t HC_CUP_SLV   = 0xCCCD; // #c89a6a kraft sleeve
static const uint16_t HC_CUP_SLVSH = 0x7A87; // #7a5238 sleeve shadow
static const uint16_t HC_CUP_TXT   = 0x38C1; // #3a1a08 sleeve label ink
static const uint16_t HC_STEAM     = 0xDEFB; // #dddddd pale steam wisp
static const uint16_t HC_VISORLEAK = 0x05FF; // #00bdff CYAN visor light leak
static const uint16_t HC_TAG       = 0xFFDC; // #fff8e0 price-tag stock
static const uint16_t HC_TAG_TXT   = 0xC9C7; // #c83a3a "$19.99" in red
static const uint16_t HC_TAG_STR   = 0x8C51; // #888888 tag string
static const uint16_t HC_GREY      = 0x39C7; // #3a3a3a badge header / outlines
static const uint16_t HC_ANT_LED   = 0xF943; // #ff2a1a antenna-poke LED (crimson)

// ── Costume variants ────────────────────────────────────────────────
// The two designed disguises from app.jsx (gr-human / gr-human-2). The varying
// fields are hair/skin/hoodie colors, the painted expression, and the two
// labels; everything else (mask tells, googly eyes, price tag, steam) is shared.
struct CostumeVariant {
  uint16_t skin, skinSh, hair;
  uint16_t hoodie, hoodieDk, hoodieLi, hoodieTr;
  uint8_t  expr;            // 0 = awkward (drooping rictus), 1 = smile (upturned)
  const char* coffeeLabel;
  const char* bubbleText;
};
// #10 — undercover: brown wig, navy hoodie, awkward. (current defaults)
static const CostumeVariant CV_UNDERCOVER = {
  0xED90, 0xBC0B, 0x3923,
  0x3A4D, 0x29CB, 0x5B51, 0x1907,
  0, "FELLOW HUMAN BEAN", "BEEP BOOP FELLOW HUMAN"
};
// #11 — "Pretending to Pay Taxes": blond #a87838 wig, maroon #7a2828 hoodie,
// lighter skin #f0c098, a smile, relabeled cup + bubble, beardier (app.jsx).
static const CostumeVariant CV_BLOND = {
  0xF613, 0xCC8C, 0xABC7,
  0x7945, 0x58E3, 0x9A49, 0x40A2,
  1, "NOT BATTERY ACID", "NICE LOCAL WEATHER"
};
static const CostumeVariant* _costumeVar = &CV_UNDERCOVER;
static uint8_t HC_EXPR = 0;   // active expression, set by applyCostumeVariant

// Swap the active costume palette to the given variant. Called at the top of
// the costume composite so every HC_* call site below renders that variant.
static void applyCostumeVariant(const CostumeVariant* v) {
  HC_SKIN = v->skin;   HC_SKIN_SH = v->skinSh;   HC_HAIR = v->hair;
  HC_HOODIE = v->hoodie; HC_HOODIE_DK = v->hoodieDk;
  HC_HOODIE_LI = v->hoodieLi; HC_HOODIE_TR = v->hoodieTr;
  HC_EXPR = v->expr;
}

// Canvas→firmware mapping. The SVG authors every costume element in absolute
// canvas pixels with the face center at (HX,HY)=(67,55). The 3D pipeline's
// onFace()/onChest() take coordinates RELATIVE to that same center, so a
// canvas point (cx,cy) on the head/chest front plane becomes onFace/onChest
// (cx-67, cy-55). These wrappers keep the geometry below readable as the SVG.
static inline V2 cvFace(float cx, float cy)  { return onFace (cx - 67.0f, cy - 55.0f); }
static inline V2 cvChest(float cx, float cy) { return onChest(cx - 67.0f, cy - 55.0f); }

// ── #human-hoodie ───────────────────────────────────────────────────
// Navy knit hoodie over the chest, hood drapes off the head sides, V-neck
// of skin, drawstrings, kangaroo pocket, a white "100% HUMAN" name badge
// (with a tiny ID photo), and a $19.99 RED price tag dangling from the
// right hood drawstring (rotated ~-15°). Canvas: HumanHoodie in gr0m.jsx.
// Rides the chest front plane (cvChest) so the whole thing leans with the
// body, like the brand bolt. Drawstrings/badge/tag use cvFace where the
// canvas anchors them near the head so they sit on the visible hood.
static void drawHumanHoodie() {
  // ── Hood drape: two wings off the sides of the head. SVG paths
  //   left  M36,38 L30,78 L38,88 L38,78 Q40,50 50,42 Z
  //   right M98,38 L104,78 L96,88 L96,78 Q94,50 84,42 Z
  // The quadratic inner edge → a 2-triangle fan that hugs the head side.
  V2 lA = cvChest(36, 38), lB = cvChest(30, 78), lC = cvChest(38, 88);
  V2 lD = cvChest(38, 78), lE = cvChest(45, 48), lF = cvChest(50, 42);
  _t->fillTriangle(lA.x, lA.y, lB.x, lB.y, lC.x, lC.y, HC_HOODIE);
  _t->fillTriangle(lA.x, lA.y, lC.x, lC.y, lD.x, lD.y, HC_HOODIE);
  _t->fillTriangle(lA.x, lA.y, lD.x, lD.y, lE.x, lE.y, HC_HOODIE);
  _t->fillTriangle(lA.x, lA.y, lE.x, lE.y, lF.x, lF.y, HC_HOODIE);
  V2 rA = cvChest(98, 38), rB = cvChest(104, 78), rC = cvChest(96, 88);
  V2 rD = cvChest(96, 78), rE = cvChest(89, 48), rF = cvChest(84, 42);
  _t->fillTriangle(rA.x, rA.y, rB.x, rB.y, rC.x, rC.y, HC_HOODIE);
  _t->fillTriangle(rA.x, rA.y, rC.x, rC.y, rD.x, rD.y, HC_HOODIE);
  _t->fillTriangle(rA.x, rA.y, rD.x, rD.y, rE.x, rE.y, HC_HOODIE);
  _t->fillTriangle(rA.x, rA.y, rE.x, rE.y, rF.x, rF.y, HC_HOODIE);

  // ── Body: rect (36,84) 62×26, fill hoodie. 1px highlight top + shadow bottom.
  V2 btl = cvChest(36, 84), btr = cvChest(98, 84);
  V2 bbr = cvChest(98, 110), bbl = cvChest(36, 110);
  _t->fillTriangle(btl.x, btl.y, btr.x, btr.y, bbr.x, bbr.y, HC_HOODIE);
  _t->fillTriangle(btl.x, btl.y, bbr.x, bbr.y, bbl.x, bbl.y, HC_HOODIE);
  _t->drawLine(btl.x, btl.y, btr.x, btr.y, HC_HOODIE_LI);   // y=84 highlight
  _t->drawLine(bbl.x, bbl.y, bbr.x, bbr.y, HC_HOODIE_TR);   // y=109 shadow

  // ── V-neck: triangle of skin showing. SVG M56,84 L67,92 L78,84 Z
  V2 vL = cvChest(56, 84), vB = cvChest(67, 92), vR = cvChest(78, 84);
  _t->fillTriangle(vL.x, vL.y, vB.x, vB.y, vR.x, vR.y, HC_SKIN);

  // ── Drawstrings: two 1px white lines + tip pixels. SVG (62,92)-(62,98),
  //   (71,92)-(71,97). On the chest plane just under the V-neck.
  V2 ds0 = cvChest(62, 92), ds1 = cvChest(62, 98);
  V2 es0 = cvChest(71, 92), es1 = cvChest(71, 97);
  _t->drawLine(ds0.x, ds0.y, ds1.x, ds1.y, HC_STRING);
  _t->drawLine(es0.x, es0.y, es1.x, es1.y, HC_STRING);
  _t->drawPixel(ds1.x, ds1.y, HC_HOODIE_LI);
  _t->drawPixel(es1.x, es1.y, HC_HOODIE_LI);

  // ── Pocket pouch: rect (50,100) 34×6, darker hoodie + 1px trim top.
  V2 pTL = cvChest(50, 100), pTR = cvChest(84, 100);
  V2 pBR = cvChest(84, 106), pBL = cvChest(50, 106);
  _t->fillTriangle(pTL.x, pTL.y, pTR.x, pTR.y, pBR.x, pBR.y, HC_HOODIE_DK);
  _t->fillTriangle(pTL.x, pTL.y, pBR.x, pBR.y, pBL.x, pBL.y, HC_HOODIE_DK);
  _t->drawLine(pTL.x, pTL.y, pTR.x, pTR.y, HC_HOODIE_TR);

  // ── "100% HUMAN" name badge: white rect (37,94) 14×6 with a grey header
  //   stripe (37,94 14×1.5), silkscreen text (too small to print legibly at
  //   this scale → a 3px grey ID line stands in), and a tiny skin ID photo
  //   (38,96) 3×3 with one brown pixel.
  V2 bgTL = cvChest(37, 94), bgTR = cvChest(51, 94);
  V2 bgBR = cvChest(51, 100), bgBL = cvChest(37, 100);
  _t->fillTriangle(bgTL.x, bgTL.y, bgTR.x, bgTR.y, bgBR.x, bgBR.y, HC_CUP);
  _t->fillTriangle(bgTL.x, bgTL.y, bgBR.x, bgBR.y, bgBL.x, bgBL.y, HC_CUP);
  _t->drawLine(bgTL.x, bgTL.y, bgTR.x, bgTR.y, HC_GREY);              // header stripe
  V2 idTL = cvChest(38, 96), idBR = cvChest(41, 99);                  // 3×3 photo
  _t->fillRect(idTL.x, idTL.y, (idBR.x - idTL.x) > 0 ? idBR.x - idTL.x : 1,
                               (idBR.y - idTL.y) > 0 ? idBR.y - idTL.y : 1, HC_SKIN);
  V2 idDot = cvChest(39, 97);
  _t->drawPixel(idDot.x, idDot.y, HC_HAIR);                           // brown hair pixel
  V2 nameL = cvChest(43, 98), nameR = cvChest(50, 98);                // "100% HUMAN" stand-in
  _t->drawLine(nameL.x, nameL.y, nameR.x, nameR.y, HC_GREY);

  // ── $19.99 price tag dangling from the right drawstring, rotated -15°.
  //   SVG <g translate(96 76) rotate(-15)> with the polygon (-2,2)(12,2)
  //   (14,8)(-4,8) and a string up to (-1,-12). We pre-rotate the local
  //   tag points by -15° about the (96,76) pivot, then map through cvChest.
  {
    const float ca = cosf(-0.2618f), sa = sinf(-0.2618f);  // -15°
    auto tagPt = [&](float lx, float ly) -> V2 {
      float rx = lx * ca - ly * sa, ry = lx * sa + ly * ca;
      return cvChest(96.0f + rx, 76.0f + ry);
    };
    V2 strTop = tagPt(-1, -12), strBot = tagPt(4, 2);
    _t->drawLine(strTop.x, strTop.y, strBot.x, strBot.y, HC_TAG_STR);
    V2 q0 = tagPt(-2, 2), q1 = tagPt(12, 2), q2 = tagPt(14, 8), q3 = tagPt(-4, 8);
    _t->fillTriangle(q0.x, q0.y, q1.x, q1.y, q2.x, q2.y, HC_TAG);
    _t->fillTriangle(q0.x, q0.y, q2.x, q2.y, q3.x, q3.y, HC_TAG);
    _t->drawLine(q0.x, q0.y, q1.x, q1.y, HC_GREY);
    _t->drawLine(q3.x, q3.y, q2.x, q2.y, HC_GREY);
    V2 hole = tagPt(-1, 3);
    _t->drawPixel(hole.x, hole.y, HC_GREY);                 // string eyelet
    // "$19.99" — too small to print; a short red bar across the tag stands in.
    V2 t0 = tagPt(0, 6), t1 = tagPt(10, 6);
    _t->drawLine(t0.x, t0.y, t1.x, t1.y, HC_TAG_TXT);
  }
}

// ── #human-face-mask ────────────────────────────────────────────────
// Flesh mask over the visor with the disguise's loudest tells: a CYAN
// visor light LEAKING out under the mask edge (y72-74), mismatched googly
// eyes (left big r6, right small r4.5) with pupils pointing different ways,
// crooked uneven marker eyebrows, a painted-on red rictus smile with a
// faint misaligned brown mouth seam underneath, and a beard PEELING off the
// left with two falling chunks. Canvas: HumanFaceMask. Front-face only.
static void drawHumanFaceMask() {
  if (!frontFaceVisible()) return;
  // Mask body. SVG path M47,44 L46,71 Q67,79 89,71 L88,44 Q67,38 47,44 Z.
  // The two quadratic curves (top brow + bottom chin) → triangle fans.
  V2 a = cvFace(47, 44), b = cvFace(46, 71);          // left edge
  V2 cQ = cvFace(67, 79);                             // bottom curve apex
  V2 d = cvFace(89, 71), e = cvFace(88, 44);          // right edge / top-right
  V2 tQ = cvFace(67, 38);                             // top curve apex
  _t->fillTriangle(a.x, a.y, b.x, b.y, cQ.x, cQ.y, HC_SKIN);
  _t->fillTriangle(a.x, a.y, cQ.x, cQ.y, d.x, d.y, HC_SKIN);
  _t->fillTriangle(a.x, a.y, d.x, d.y, e.x, e.y, HC_SKIN);
  _t->fillTriangle(a.x, a.y, e.x, e.y, tQ.x, tQ.y, HC_SKIN);
  // Mask edge shadow stroke along the brow + chin curve.
  _t->drawLine(a.x, a.y, b.x, b.y, HC_SKIN_SH);
  _t->drawLine(b.x, b.y, cQ.x, cQ.y, HC_SKIN_SH);
  _t->drawLine(cQ.x, cQ.y, d.x, d.y, HC_SKIN_SH);
  _t->drawLine(d.x, d.y, e.x, e.y, HC_SKIN_SH);

  // ── CYAN VISOR LIGHT LEAKING BELOW the mask (y72-74). THIS IS CRUCIAL.
  // SVG: (48,72) 1×2, (87,72) 1×2, (49,74) 3×1 in #00bdff (varying opacity →
  // we drop the dim middle one a half-step toward visor-off for the falloff).
  V2 lk0 = cvFace(48, 72), lk1 = cvFace(48, 73);
  V2 lk2 = cvFace(87, 72), lk3 = cvFace(87, 73);
  _t->drawPixel(lk0.x, lk0.y, HC_VISORLEAK);
  _t->drawPixel(lk1.x, lk1.y, HC_VISORLEAK);
  _t->drawPixel(lk2.x, lk2.y, HC_VISORLEAK);
  _t->drawPixel(lk3.x, lk3.y, VISOR_OFF);       // dimmer right leak
  V2 lkA = cvFace(49, 74), lkB = cvFace(52, 74);
  _t->drawLine(lkA.x, lkA.y, lkB.x, lkB.y, HC_VISORLEAK);

  // ── Cheek shadows: two ellipses at (52,62) and (82,62), rx3 ry2.
  V2 chL = cvFace(52, 62), chR = cvFace(82, 62);
  _t->fillEllipse(chL.x, chL.y, pkS(3), pkS(2), HC_SKIN_SH);
  _t->fillEllipse(chR.x, chR.y, pkS(3), pkS(2), HC_SKIN_SH);

  // ── UNEVEN MARKER EYEBROWS: left (50,47) 11×1.5 + tail (49,48) 2×1,
  //   right (73,48) 11×1.5 — deliberately at different heights.
  V2 ebL0 = cvFace(50, 47), ebL1 = cvFace(61, 47);
  V2 ebR0 = cvFace(73, 48), ebR1 = cvFace(84, 48);
  _t->drawLine(ebL0.x, ebL0.y, ebL1.x, ebL1.y, HC_EYEBROW);
  _t->drawLine(ebL0.x, ebL0.y + 1, ebL1.x, ebL1.y + 1, HC_EYEBROW);
  _t->drawLine(ebR0.x, ebR0.y, ebR1.x, ebR1.y, HC_EYEBROW);
  _t->drawLine(ebR0.x, ebR0.y + 1, ebR1.x, ebR1.y + 1, HC_EYEBROW);
  V2 ebTail0 = cvFace(49, 48), ebTail1 = cvFace(51, 48);
  _t->drawLine(ebTail0.x, ebTail0.y, ebTail1.x, ebTail1.y, HC_EYEBROW);

  // ── GOOGLY EYES — MISMATCHED. Left big (56,54) r6, right small (79,54)
  //   r4.5. Pupils: left dropped down-left (53,57) r2.5, right up-right
  //   (80.5,52.5) r2. White speck on each.
  V2 elc = cvFace(56, 54), erc = cvFace(79, 54);
  int elr = pkS(6), err = pkS(5); if (elr < 2) elr = 2; if (err < 2) err = 2;
  _t->fillCircle(elc.x, elc.y, elr, HC_CUP);
  _t->drawCircle(elc.x, elc.y, elr, INK);
  _t->fillCircle(erc.x, erc.y, err, HC_CUP);
  _t->drawCircle(erc.x, erc.y, err, INK);
  V2 plc = cvFace(53, 57), prc = cvFace(81, 53);   // 80.5,52.5 rounded
  int plr = pkS(3), prr = pkS(2); if (plr < 1) plr = 1; if (prr < 1) prr = 1;
  _t->fillCircle(plc.x, plc.y, plr, INK);
  _t->fillCircle(prc.x, prc.y, prr, INK);
  V2 spL = cvFace(52, 55), spR = cvFace(80, 51);
  _t->drawPixel(spL.x, spL.y, HC_CUP);
  _t->drawPixel(spR.x, spR.y, HC_CUP);

  // ── Crooked nose: rect (65,59) 2×4 + bridge pixel (64,62) wide shelf.
  V2 nT = cvFace(66, 59), nB = cvFace(66, 63);
  _t->drawLine(nT.x, nT.y, nB.x, nB.y, HC_SKIN_SH);
  V2 nL = cvFace(64, 62), nR = cvFace(68, 62);
  _t->drawLine(nL.x, nL.y, nR.x, nR.y, HC_SKIN_SH);

  // ── PAINTED-ON RED RICTUS SMILE. SVG M56,67 Q67,73 78,67 (red, 1.6px).
  //   Quadratic → 4 chords through the Q control (67,73) midpoint sag. The
  //   blond variant's 'smile' expression droops less (sag 3) than the
  //   'awkward' undercover rictus (sag 5).
  int sag = HC_EXPR ? 3 : 5;
  V2 sm0 = cvFace(56, 67), sm1 = cvFace(62, 66 + sag), sm2 = cvFace(67, 67 + sag);
  V2 sm3 = cvFace(72, 66 + sag), sm4 = cvFace(78, 67);
  _t->drawLine(sm0.x, sm0.y, sm1.x, sm1.y, HC_MOUTH);
  _t->drawLine(sm1.x, sm1.y, sm2.x, sm2.y, HC_MOUTH);
  _t->drawLine(sm2.x, sm2.y, sm3.x, sm3.y, HC_MOUTH);
  _t->drawLine(sm3.x, sm3.y, sm4.x, sm4.y, HC_MOUTH);
  _t->drawLine(sm0.x, sm0.y + 1, sm2.x, sm2.y + 1, HC_MOUTH);  // ~1.6px weight
  _t->drawLine(sm2.x, sm2.y + 1, sm4.x, sm4.y + 1, HC_MOUTH);

  // ── Faint MISALIGNED mouth seam underneath. SVG M58,68 Q67,71 76,68
  //   (brown 0.5px) — shorter + offset so it doesn't line up with the smile.
  V2 se0 = cvFace(58, 68), se1 = cvFace(67, 70), se2 = cvFace(76, 68);
  _t->drawLine(se0.x, se0.y, se1.x, se1.y, HC_SEAM);
  _t->drawLine(se1.x, se1.y, se2.x, se2.y, HC_SEAM);

  // ── PEELING BEARD on the LEFT side. Two attached chunks (52,71) 6×3 +
  //   (54,73) 2×2, plus TWO FALLING chunks rotated -15° @ (50,70) and
  //   -25° @ (49,72). We map the falling chunks' rotated corners.
  V2 bc0 = cvFace(52, 71), bc1 = cvFace(58, 74);
  _t->fillRect(bc0.x, bc0.y, (bc1.x - bc0.x) > 0 ? bc1.x - bc0.x : 1,
                             (bc1.y - bc0.y) > 0 ? bc1.y - bc0.y : 1, HC_HAIR);
  V2 bc2 = cvFace(54, 73), bc3 = cvFace(56, 75);
  _t->fillRect(bc2.x, bc2.y, (bc3.x - bc2.x) > 0 ? bc3.x - bc2.x : 1,
                             (bc3.y - bc2.y) > 0 ? bc3.y - bc2.y : 1, HC_HAIR);
  auto chunk = [&](float px, float py, float w, float h, float deg) {
    const float ca = cosf(deg * 0.01745f), sa = sinf(deg * 0.01745f);
    auto rot = [&](float lx, float ly) -> V2 {
      float rx = lx * ca - ly * sa, ry = lx * sa + ly * ca;
      return cvFace(px + rx, py + ry);
    };
    V2 q0 = rot(0, 0), q1 = rot(w, 0), q2 = rot(w, h), q3 = rot(0, h);
    _t->fillTriangle(q0.x, q0.y, q1.x, q1.y, q2.x, q2.y, HC_HAIR);
    _t->fillTriangle(q0.x, q0.y, q2.x, q2.y, q3.x, q3.y, HC_HAIR);
  };
  chunk(50, 70, 3, 2, -15);   // first falling chunk
  chunk(49, 72, 2, 3, -25);   // second falling chunk, peeling further
  if (HC_EXPR) chunk(47, 74, 2, 3, -32);   // blond: beard peels further (canvas sub)
}

// ── #human-wig ──────────────────────────────────────────────────────
// Brown wig sitting ASKEW: (-3,+2) px translate + ~-4° lean. Covers the top
// + sides of the head with stray cowlick flicks on top. Canvas: HumanWig.
// SVG main path M40,32 L40,50 L42,50 L43,42 L47,36 L55,33 L67,31 L79,33
//   L86,36 L90,42 L91,50 L94,50 L94,32 Q70,22 40,32 Z (rotate -4° about
//   67,35, translate -3,+2). Front-face only.
static void drawHumanWig() {
  if (!frontFaceVisible()) return;
  // Pre-apply the askew transform in canvas space: rotate -4° about (67,35),
  // then translate (-3,+2). wp() maps a wig-local canvas point onto the face.
  const float ca = cosf(-0.0698f), sa = sinf(-0.0698f);  // -4°
  auto wp = [&](float cx, float cy) -> V2 {
    float dx = cx - 67.0f, dy = cy - 35.0f;
    float rx = dx * ca - dy * sa, ry = dx * sa + dy * ca;
    return cvFace(67.0f + rx - 3.0f, 35.0f + ry + 2.0f);
  };
  // Main mass: fan the outline from the crown apex (67,22, the Q control) so
  // the curved top reads as a rounded hairline. Build the bottom hairline
  // from the path's L-segments, sweeping left→right.
  V2 apex = wp(67, 23);
  static const float HL[][2] = {
    {40,50},{42,50},{43,42},{47,36},{55,33},{67,31},
    {79,33},{86,36},{90,42},{91,50},{94,50}
  };
  for (int i = 0; i + 1 < (int)(sizeof(HL)/sizeof(HL[0])); i++) {
    V2 p0 = wp(HL[i][0],   HL[i][1]);
    V2 p1 = wp(HL[i+1][0], HL[i+1][1]);
    _t->fillTriangle(apex.x, apex.y, p0.x, p0.y, p1.x, p1.y, HC_HAIR);
  }
  // Close the rounded top: outer corners to the apex.
  V2 oL = wp(40, 32), oR = wp(94, 32);
  _t->fillTriangle(apex.x, apex.y, oL.x, oL.y, wp(40, 50).x, wp(40, 50).y, HC_HAIR);
  _t->fillTriangle(apex.x, apex.y, oR.x, oR.y, wp(94, 50).x, wp(94, 50).y, HC_HAIR);
  // Side tabs over the temples. SVG (42,48) 3×6 + (91,48) 3×6.
  V2 stL0 = wp(42, 48), stL1 = wp(45, 54);
  V2 stR0 = wp(91, 48), stR1 = wp(94, 54);
  _t->fillRect(stL0.x, stL0.y, (stL1.x-stL0.x)>0?stL1.x-stL0.x:1, (stL1.y-stL0.y)>0?stL1.y-stL0.y:1, HC_HAIR);
  _t->fillRect(stR0.x, stR0.y, (stR1.x-stR0.x)>0?stR1.x-stR0.x:1, (stR1.y-stR0.y)>0?stR1.y-stR0.y:1, HC_HAIR);
  // Stray cowlick flicks on top — 4 positions per the SVG (last two fainter).
  static const float COW[][2] = { {50,26},{51,25},{58,27},{80,27} };
  for (int i = 0; i < 4; i++) {
    V2 f0 = wp(COW[i][0], COW[i][1]);
    V2 f1 = wp(COW[i][0], COW[i][1] - 3);
    _t->drawLine(f0.x, f0.y, f1.x, f1.y, HC_HAIR);
  }
  V2 cwa = wp(62, 28), cwb = wp(72, 28);   // two faint top flicks
  _t->drawPixel(cwa.x, cwa.y, HC_HAIR);
  _t->drawPixel(cwb.x, cwb.y, HC_HAIR);
}

// ── #antenna-poke ───────────────────────────────────────────────────
// The antenna shaft poking UP THROUGH the wig at top center, capped by an
// LED ball with a halo glow. Canvas: AntennaPoke. SVG line (71,28)-(73,16),
// LED circle r2 @ (73,16) with r3/r5 halo rings + a white speck (72,15).
// Drawn AFTER the wig so the shaft visibly emerges through the hair.
static void drawAntennaPoke(uint16_t led) {
  if (!frontFaceVisible()) return;
  V2 base = cvFace(71, 28), tip = cvFace(73, 16);
  _t->drawLine(base.x, base.y, tip.x, tip.y, CHASSIS_SH);
  _t->drawLine(base.x + 1, base.y, tip.x + 1, tip.y, CHASSIS_SH);
  int lr = pkS(2); if (lr < 1) lr = 1;
  _t->drawCircle(tip.x, tip.y, lr + 3, (led >> 2) & 0x39E7);   // outer halo (r5)
  _t->drawCircle(tip.x, tip.y, lr + 1, (led >> 1) & 0x7BEF);   // inner halo (r3)
  _t->fillCircle(tip.x, tip.y, lr, led);                       // LED ball (r2)
  _t->drawCircle(tip.x, tip.y, lr, SPECULAR);
  V2 spk = cvFace(72, 15);
  _t->drawPixel(spk.x, spk.y, SPECULAR);                       // catchlight
}

// ── #coffee-cup ─────────────────────────────────────────────────────
// White takeaway cup with a brown rim + side outlines, a kraft sleeve
// labeled "FELLOW HUMAN BEAN", and rising steam. Canvas: CoffeeCup. The cup
// rides the chest plane (cvChest) so it leans with the body. `t` animates
// the steam. Cup body SVG (92,94) 10×14.
static void drawCoffeeCup(const char* label, uint32_t t) {
  // Body: rect (92,94) 10×14 white.
  V2 cTL = cvChest(92, 94), cTR = cvChest(102, 94);
  V2 cBR = cvChest(102, 108), cBL = cvChest(92, 108);
  _t->fillTriangle(cTL.x, cTL.y, cTR.x, cTR.y, cBR.x, cBR.y, HC_CUP);
  _t->fillTriangle(cTL.x, cTL.y, cBR.x, cBR.y, cBL.x, cBL.y, HC_CUP);
  // Brown rim (92,94 10×2) + side outlines (91/102, 96, 1×10).
  _t->drawLine(cTL.x, cTL.y, cTR.x, cTR.y, HC_CUP_TOP);
  _t->drawLine(cTL.x, cTL.y + 1, cTR.x, cTR.y + 1, HC_CUP_TOP);
  _t->drawLine(cTL.x, cTL.y, cBL.x, cBL.y, HC_SEAM);
  _t->drawLine(cTR.x, cTR.y, cBR.x, cBR.y, HC_SEAM);
  // Lid: (92,92) 10×2 + tab (95,91) 4×1.
  V2 ldL = cvChest(92, 92), ldR = cvChest(102, 92);
  _t->drawLine(ldL.x, ldL.y, ldR.x, ldR.y, HC_CUP_TOP);
  V2 tbL = cvChest(95, 91), tbR = cvChest(99, 91);
  _t->drawLine(tbL.x, tbL.y, tbR.x, tbR.y, HC_CUP_TOP);
  // Kraft sleeve: (92,100) 10×4 + 1px shadow top, with the silkscreen label.
  V2 svTL = cvChest(92, 100), svTR = cvChest(102, 100);
  V2 svBR = cvChest(102, 104), svBL = cvChest(92, 104);
  _t->fillTriangle(svTL.x, svTL.y, svTR.x, svTR.y, svBR.x, svBR.y, HC_CUP_SLV);
  _t->fillTriangle(svTL.x, svTL.y, svBR.x, svBR.y, svBL.x, svBL.y, HC_CUP_SLV);
  _t->drawLine(svTL.x, svTL.y, svTR.x, svTR.y, HC_CUP_SLVSH);
  // Sleeve label "FELLOW HUMAN BEAN" — silkscreen 2.4px on the canvas is far
  // below the 6px ROM font, so we silkscreen the FIRST char as a label cue +
  // a hint bar (full text would be unreadable at sprite scale).
  if (label && label[0]) {
    _t->setTextSize(1);
    _t->setTextColor(HC_CUP_TXT, HC_CUP_SLV);
    char cue[2] = { label[0], 0 };
    _t->setCursor(svTL.x + 1, svTL.y - 1);
    _t->print(cue);
  }
  // Steam: 3 dotted lines rising + drifting (anti-gravity wisps). SVG dots at
  // x≈95/97/99, y85-90, varying opacity.
  for (int i = 0; i < 3; i++) {
    int phase = ((int)(t / 120) + i * 4) % 12;
    V2 s = cvChest(95.0f + i * 2.0f, 90.0f - phase);
    if (s.y < 1) continue;
    _t->drawPixel(s.x, s.y, HC_STEAM);
    _t->drawPixel(s.x + ((phase & 1) ? 1 : -1), s.y - 1, HC_STEAM);
  }
}

// ── #human-bubble ───────────────────────────────────────────────────
// "BEEP BOOP FELLOW HUMAN" speech bubble with a tail pointing back-left at
// the head. Canvas: HumanBubble — white rounded rect at (8,14) w≈86 h11,
// tail at (22-28,25-30). This is a HUD overlay (NOT on a 3D plane) so it is
// drawn in absolute screen space, like drawSpeechBubble. Clamps to width.
static void drawHumanBubble(const char* text) {
  int len = 0; while (text && text[len]) len++;
  int w = len * 6 + 6;            // 6px ROM font, vs the canvas's 4.5px silkscreen
  if (w > 132) w = 132;
  int x = 2, y = 14;
  _t->fillRoundRect(x, y, w, 12, 2, SPECULAR);
  _t->drawRoundRect(x, y, w, 12, 2, INK);
  // Tail pointing down-left back toward the head (canvas tail at x+14..x+20).
  int tx = x + 14;
  _t->fillTriangle(tx, y + 11, tx + 6, y + 11, tx + 4, y + 16, SPECULAR);
  _t->drawLine(tx, y + 11, tx + 4, y + 16, INK);
  _t->drawLine(tx + 6, y + 11, tx + 4, y + 16, INK);
  _t->setTextSize(1);
  _t->setTextColor(INK, SPECULAR);
  _t->setCursor(x + 3, y + 3);
  if (text) _t->print(text);
}

// ── #human-costume ──────────────────────────────────────────────────
// The Stage-5 endgame composite — AGI cosplaying a human. Composed BACK TO
// FRONT exactly as the canvas: hoodie → face mask → wig → antenna-poke →
// coffee → bubble. All six detail elements render together. Mirrors the
// HumanCostume defaults (coffee/bubble/price-tag/antenna all on).
static void drawHumanCostumeFull(uint32_t t, bool showBubble) {
  applyCostumeVariant(_costumeVar);                  // swap palette to active variant
  drawHumanHoodie();                                 // 4. hoodie + badge + price tag
  drawHumanFaceMask();                               // 3. mask + visor leak + googly eyes + beard
  drawHumanWig();                                    // 1. wig askew + cowlicks
  drawAntennaPoke(HC_ANT_LED);                       // 2. antenna poking UP THROUGH the wig
  drawCoffeeCup(_costumeVar->coffeeLabel, t);        // 5. coffee cup
  if (showBubble)                                    // 6. speech bubble (suppressed
    drawHumanBubble(_costumeVar->bubbleText);        //    in attention to keep the "!" alert)
}

// Human-costume scene body (the A+B manual toggle target) — composed onto
// whatever surface _t points at. Called from the global trampoline below
// (which sets _t = tgt first). Draws the base bot first (so the square head
// corners + rivets peek out past the disguise and the cyan visor can leak
// below the mask edge), then layers the full canvas costume. `t` is millis().
static void humanCostumeScene(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawShadow(0);
  // Base bot underneath — head edges + chassis rivets stay visible.
  drawChest3D();
  drawBolt3D(VISOR_IDLE);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_IDLE);   // cyan visor under the mask — leaks below the edge
  // NB: NO base drawAntenna3D() here. The costume's antenna is the antenna-poke
  // (drawn through the wig in drawHumanCostumeFull). The old base stub pushed a
  // SECOND dark pole + dark LED ring up beside the lit poke LED, so two antenna
  // tips collided at the top — that doubled/ghost antenna is the "top looks bad".
  // Costume on top — the full canvas composite (hoodie → mask → wig →
  // antenna-poke → coffee → bubble).
  drawHumanCostumeFull(t);
  // No mood motes here either — they flickered beside the disguise (see doSleep note).
}

// ════════════════════════════════════════════════════════════════════
//   DJ MODE — "gr0m on the decks" booth scene (bonus, not a mood-state)
// ────────────────────────────────────────────────────────────────────
//   A standalone full-detail scene driven by a deterministic internal
//   beat (~124 BPM). NOT evolution-gated — it's a mode, not a pet stage.
//   The beat envelope (1.0 on the kick, decaying to ~0 between beats)
//   drives the head-bob, VU-bar heights, antenna pulse, crossfader slide
//   and the right-deck "scratch" jitter. No mic / no real audio.
// ════════════════════════════════════════════════════════════════════

static const uint32_t DJ_BPM       = 124;
static const uint32_t DJ_BEAT_MS   = 60000UL / DJ_BPM;   // ~484 ms per beat

// Club-lighting palette the VU bars + notes cycle through.
static const uint16_t DJ_COLORS[6] = {
  0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x05FF, 0xF81F
};

// Beat envelope in 0..255: 255 right on the kick, decaying toward 0
// across the beat. `phaseMs` is how far we are into the current beat.
static uint8_t djBeatEnvelope(uint32_t phaseMs) {
  // Linear decay over the first ~70% of the beat, flat-zero after.
  uint32_t span = (DJ_BEAT_MS * 7) / 10;
  if (phaseMs >= span) return 0;
  return (uint8_t)(255 - (phaseMs * 255) / span);
}

// Club-light palette for the VU bars (canvas DJVuVisor `colors[]`), translated
// to RGB565: #ff2a1a #ff8c1a #ffd60a #29d65b #00bdff #5f4cff #ff2d92.
// These are the HEART_RED/EMBER_HOT/YELLOW/GREEN/CYAN/PURPLE/MAGENTA the SPEC
// names; gr0m.cpp has no EMBER_HOT/YELLOW/etc. macros so they live here.
static const uint16_t DJ_VU[7] = {
  0xF943, 0xFC63, 0xFEA1, 0x2EAB, 0x05FF, 0x5A7F, 0xF972
};
// Note-particle palette (canvas DJNotes `colors[]`): magenta, cyan, green,
// yellow, orange.
static const uint16_t DJ_NOTE_COL[5] = {
  0xF972, 0x05FF, 0x2EAB, 0xFEA1, 0xFC63
};

// ── Neon DJ-booth palette + color math ──────────────────────────────
// "Late-night neon booth": near-black stage, deep indigo/violet glow, and a
// cyan/magenta/hot-pink neon accent set the head rim, antenna strobe, deck
// glints and EQ peaks all share. GFX has no alpha, so depth is faked with
// STEPPED fills, an ordered dither between steps, and saturating additive
// blends for the beat flare.
static const uint16_t NEON_CYAN   = 0x3F3F;  // rgb(60,230,255)  rim / accent
static const uint16_t NEON_MAGENTA= 0xFA3A;  // rgb(255,70,210)  rim / accent
static const uint16_t NEON_PINK   = 0xF9F1;  // rgb(255,60,140)  hot accent
// Antenna strobe walks this tight neon set (no warm/green — keep it "club").
static const uint16_t NEON_STROBE[4] = { 0x3F3F, 0xF81F, 0xFA3A, 0x7C9F };

// Stage-glow bands, near-black floor → violet core (index 0 darkest). The DJ
// scene paints these as concentric/vertical fills behind the booth and adds a
// decaying flare on the kick.
static const uint16_t GLOW_BAND[4] = {
  0x0001,  // near-black  rgb(6,2,14)
  0x0823,  // deep indigo rgb(14,4,30)
  0x1846,  // indigo      rgb(26,8,52)
  0x2869,  // violet core rgb(44,14,78)
};

// Split an RGB565 into 8-bit channels and back.
static inline void rgb565split(uint16_t c, int& r, int& g, int& b) {
  r = ((c >> 11) & 0x1F) << 3;
  g = ((c >> 5)  & 0x3F) << 2;
  b = ( c        & 0x1F) << 3;
}
static inline uint16_t rgb565pack(int r, int g, int b) {
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
// Saturating additive blend — fakes a glow "lighting up" without alpha.
static inline uint16_t rgb565add(uint16_t base, uint16_t add, int num, int den) {
  int br, bg, bb, ar, ag, ab;
  rgb565split(base, br, bg, bb);
  rgb565split(add,  ar, ag, ab);
  return rgb565pack(br + ar * num / den, bg + ag * num / den, bb + ab * num / den);
}
// 4x4 ordered (Bayer) dither threshold, 0..15 — used to soften the hard step
// between two glow bands so the stage looks like a gradient, not stripes.
static inline int bayer4(int x, int y) {
  static const uint8_t M[16] = { 0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5 };
  return M[(y & 3) * 4 + (x & 3)];
}

// Shared 0..255 "kick flare" the whole scene reacts to. djMusicScene snaps it
// to 255 on a beat and decays it each frame; the stage glow brightens, the
// head rim flushes, and the EQ swells off this one envelope so the scene
// pulses as a single body.
static uint8_t _djFlare = 0;

// Filled ellipse CLAMPED to a vertical band [clipTop, clipBot). The stock
// fillEllipse can't clip, and the stage-glow pool is centered low + wide, so
// without this it would spill below the buddy zone (y150 footer). Cheap
// scanline rasterizer, only over rows inside the clip band.
static void fillEllipseClip(int cx, int cy, int rx, int ry, uint16_t c,
                            int clipTop, int clipBot) {
  if (rx <= 0 || ry <= 0) return;
  int y0 = cy - ry; if (y0 < clipTop) y0 = clipTop;
  int y1 = cy + ry; if (y1 > clipBot - 1) y1 = clipBot - 1;
  for (int y = y0; y <= y1; y++) {
    float ny = (float)(y - cy) / (float)ry;
    float t = 1.0f - ny * ny; if (t <= 0.0f) continue;
    int half = (int)(rx * sqrtf(t) + 0.5f);
    _t->drawFastHLine(cx - half, y, half * 2 + 1, c);
  }
}

// ── Stage glow ──────────────────────────────────────────────────────
// Paint the near-black booth atmosphere: a soft radial-ish pool of light
// rising from behind the decks. No alpha — we draw concentric rounded bands
// of GLOW_BAND (darkest outside → violet core low-center), ordered-dither the
// seam between two bands, then ADD a decaying violet flare on the kick. Cheap:
// a handful of fills + one dithered seam pass, all CLAMPED to the buddy zone
// so the pool never spills past the y150 HUD footer.
//   zTop/zBot — vertical extent of the glow region (the buddy zone).
// `paneW`>0 + `paneCx`>=0 retarget the glow to a narrower, off-center pane
// (the 110px landscape-clock pane, core under x≈55) instead of the full 135px
// canvas; the defaults keep the HOME booth byte-for-byte unchanged.
static void drawStageGlow(int zTop, int zBot, int paneW = 0, int paneCx = -1) {
  const int W = (paneW > 0) ? paneW : BUDDY_CANVAS_W;
  // Flare 0..255 → 0..1 boost applied to the glow color (additive, saturating).
  int flare = _djFlare;
  // 1) Flat near-black wash over the whole zone (band 0).
  uint16_t b0 = GLOW_BAND[0];
  if (flare) b0 = rgb565add(b0, GLOW_BAND[3], flare, 900);
  _t->fillRect(0, zTop, W, zBot - zTop, b0);

  // 2) A pool of light BEHIND the EQ centerpiece. Concentric ELLIPSES, big →
  //    small, brightening toward the core. The core sits low (right behind the
  //    EQ bars) so the brightest part of the stage backlights the hero meter,
  //    not the empty middle — the light reads as rising up through the booth.
  const int cx = (paneCx >= 0) ? paneCx : W / 2;
  const int cy = zBot - 18;            // pool core behind the EQ bars (~y130)
  // (rx, ry, band-index) outer→inner. Bands brighten as we move in.
  const int rings[3][3] = {
    { 100, 78, 1 },  // wide deep-indigo haze fills the zone
    {  72, 54, 2 },  // indigo mid
    {  46, 34, 3 },  // violet core, backlighting the EQ
  };
  for (int k = 0; k < 3; k++) {
    uint16_t c = GLOW_BAND[rings[k][2]];
    if (flare) c = rgb565add(c, GLOW_BAND[3], flare, 600);  // flare lifts the pool
    fillEllipseClip(cx, cy, rings[k][0], rings[k][1], c, zTop, zBot);
  }

  // 3) Ordered-dither the seam between the violet core and the indigo mid so
  //    the step reads as a gradient. Sprinkle core-color pixels in the mid ring
  //    by the Bayer pattern (denser near the core edge).
  uint16_t core = GLOW_BAND[3];
  if (flare) core = rgb565add(core, GLOW_BAND[3], flare, 600);
  for (int y = cy - 48; y <= cy + 36; y++) {
    if (y < zTop || y >= zBot) continue;
    for (int x = cx - 64; x <= cx + 64; x++) {
      if (x < 0 || x >= W) continue;
      // distance on a ring between the mid + core ellipses (rx≈58, ry≈44)
      float nx = (float)(x - cx) / 58.0f, ny = (float)(y - cy) / 44.0f;
      float d = nx * nx + ny * ny;
      if (d > 0.55f && d < 1.05f && bayer4(x, y) < 6) _t->drawPixel(x, y, core);
    }
  }
}

// ── Neon head rim-light ─────────────────────────────────────────────
// A 1px cyan/magenta highlight edge tracing the head's front-face silhouette
// so the booth's neon reads as actually LIGHTING the chassis. Runs through the
// SAME rp() projection + DJ head transform as the head, so it stays glued to
// the shrunk DJ head. Left/top edge catches the cyan key-light; right/bottom
// edge catches a magenta fill — and both flush brighter on the kick (_djFlare).
// Call AFTER the head + face so the rim sits crisply on the chassis outline.
static void drawHeadRim() {
  if (!frontFaceVisible()) return;
  // Front-face corners (object space, z=+22 like the face plane), inset 1px
  // from the true edge so the neon hugs the chassis rather than floating off.
  const float hx = 21.0f, hy = 19.0f, z = 22.0f;
  V2 tl = rp({ -hx, -hy, z });
  V2 tr = rp({  hx, -hy, z });
  V2 br = rp({  hx,  hy, z });
  V2 bl = rp({ -hx,  hy, z });
  // Flare brightens the rim toward white on the kick.
  uint16_t cyan = NEON_CYAN, mag = NEON_MAGENTA;
  if (_djFlare) {
    cyan = rgb565add(cyan, 0xFFFF, _djFlare, 700);
    mag  = rgb565add(mag,  0xFFFF, _djFlare, 700);
  }
  // Top + left edges = cyan key light; right + bottom = magenta fill light.
  _t->drawLine(tl.x, tl.y, tr.x, tr.y, cyan);   // top
  _t->drawLine(tl.x, tl.y, bl.x, bl.y, cyan);   // left
  _t->drawLine(tr.x, tr.y, br.x, br.y, mag);    // right
  _t->drawLine(bl.x, bl.y, br.x, br.y, mag);    // bottom
  // Corner glints — a single bright pixel where the two rim colors meet, so the
  // edges read as lit tubing rather than a flat outline.
  _t->drawPixel(tl.x, tl.y, SPECULAR);
  _t->drawPixel(tr.x, tr.y, SPECULAR);
}

// One turntable — mirrors canvas DJTurntable(x, y, angle, label). Platter
// origin = (x, y). 11px-radius platter, three vinyl grooves, a red center
// label carrying the deck letter, a rotating radial mark, a fixed tonearm
// sweep, and the spindle. `angle` is in DEGREES (canvas SVG rotate units).
static void drawDJTurntable(int x, int y, float angle, char label) {
  // Plinth shadow (canvas: ellipse cx0 cy1 rx12 ry3, dark).
  _t->fillEllipse(x, y + 1, 12, 3, 0x10A2);
  // Platter body + rim (#1a1d20 fill, #3a3d40 rim).
  _t->fillCircle(x, y, 11, 0x18E4);
  _t->drawCircle(x, y, 11, 0x39E8);
  // Vinyl grooves at r=9/7/5 (#0a0d10).
  _t->drawCircle(x, y, 9, 0x0862);
  _t->drawCircle(x, y, 7, 0x0862);
  _t->drawCircle(x, y, 5, 0x0862);
  // Center label — gr0m red disc (#ff2a1a) + white ring + deck letter.
  _t->fillCircle(x, y, 3, 0xF943);
  _t->drawCircle(x, y, 3, SPECULAR);
  // Rotating radial mark: a short spoke from r=9→r=5 at the top of the
  // platter, rotated by `angle`. Canvas line (0,-9)->(0,-5) under rotate().
  float a = angle * 0.01745329f;          // deg → rad
  float s = sinf(a), c = cosf(a);
  // (0,-9) and (0,-5) rotated: x' = -origY*sin, y' = origY*cos.
  int x1 = x + (int)lroundf(-(-9.0f) * s), y1 = y + (int)lroundf((-9.0f) * c);
  int x2 = x + (int)lroundf(-(-5.0f) * s), y2 = y + (int)lroundf((-5.0f) * c);
  _t->drawLine(x1, y1, x2, y2, SPECULAR);
  _t->drawPixel(x1, y1, 0xFEA1);          // yellow tip pixel (#ffd60a)
  // Tonearm — fixed diagonal from the rim toward the spindle (#888) + pivot.
  _t->drawLine(x + 9, y - 9, x + 3, y - 3, 0x8C51);
  _t->fillCircle(x + 9, y - 9, 1, 0xAD55);
  // Spindle.
  _t->fillCircle(x, y, 1, INK);
  // Deck letter centered on the label (drawn last so it sits on top).
  char buf[2] = { label, 0 };
  _t->setTextSize(1);
  _t->setTextColor(SPECULAR, 0xF943);
  _t->setCursor(x - 2, y - 3);
  _t->print(buf);
}

// The mixer between the two decks — mirrors canvas DJMixer(x, y, cross).
// 20×24 body centered at (x, y): two vertical channel-fader slots + caps,
// two EQ knobs, a horizontal crossfader slot whose red cap slides by `cross`,
// and four LED level indicators.
static void drawDJMixer(int x, int y, int cross) {
  // Body (#2a2d30) with a lit top edge (#5a5d60) and dark bottom edge.
  _t->fillRect(x - 10, y - 12, 20, 24, 0x2966);
  _t->drawFastHLine(x - 10, y - 12, 20, 0x5AEC);
  _t->drawFastHLine(x - 10, y + 11, 20, 0x0841);
  // Channel faders — two vertical slots (#0a0a0a) with yellow caps.
  _t->drawFastVLine(x - 7, y - 9, 10, 0x0841);
  _t->drawFastVLine(x + 6, y - 9, 10, 0x0841);
  _t->fillRect(x - 8, y - 7, 3, 2, 0xFEA1);   // left cap (#ffd60a)
  _t->fillRect(x + 5, y - 5, 3, 2, 0xFEA1);   // right cap
  // EQ knobs (#444) with a ring + a white indicator tick.
  _t->fillCircle(x - 4, y - 8, 1, 0x4228);
  _t->drawPixel(x - 4, y - 9, SPECULAR);
  _t->fillCircle(x + 3, y - 8, 1, 0x4228);
  _t->drawPixel(x + 3, y - 9, SPECULAR);
  // Crossfader slot (16×~2) + red cap sliding on `cross` (#ff2a1a).
  _t->fillRect(x - 8, y + 4, 16, 2, 0x0841);
  _t->fillRect(x - 1 + cross, y + 3, 3, 4, 0xF943);
  // Level LEDs — green over yellow on each side (#29d65b / #ffd60a).
  _t->drawPixel(x - 8, y - 1, 0x2EAB);
  _t->drawPixel(x - 8, y + 1, 0xFEA1);
  _t->drawPixel(x + 7, y - 1, 0x2EAB);
  _t->drawPixel(x + 7, y + 1, 0xFEA1);
}

// Two hands reaching DOWN from gr0m's body onto the decks — this is what
// sells "gr0m is DJing", not just a floating head above the gear. Authored
// in the drawHandsAtLaptop / Hands(pose) idiom (chassis-shadow forearm stubs
// capped by a chassis-color knuckle circle + highlight), but anchored to the
// deck platters: the left hand rests on deck A, the right hand on deck B, and
// the forearms angle UP toward the head band so the figure reads as one body.
//   handY  — screen-Y the knuckles rest at (just above the platter centers)
//   shoulderY — where the forearms tuck up under the head/body
//   lx, rx — platter centers (deck A / deck B)
//   scratchPx — the live deck-B scratch travel (px); the right hand rides it
//               so the scratching hand visibly jolts with the platter.
static void drawDJHands(int lx, int rx, int handY, int shoulderY, int scratchPx) {
  const uint16_t ARM = CHASSIS_SH;
  const int hr = 3;
  // Shoulders sit inboard (near the body centerline); hands splay out to the
  // platters. Clamp the scratch nudge so the hand stays on its deck.
  int sgn = scratchPx >= 0 ? 1 : -1;
  int rNudge = scratchPx; if (rNudge > 4) rNudge = 4; if (rNudge < -4) rNudge = -4;
  (void)sgn;
  int lShoX = HX - 10, rShoX = HX + 10;

  // ── Left hand on deck A ──
  _t->drawLine(lShoX,     shoulderY, lx + 1, handY - 2, ARM);
  _t->drawLine(lShoX - 1, shoulderY, lx,     handY - 2, ARM);   // 2px forearm
  _t->fillCircle(lx, handY, hr, CHASSIS);
  _t->drawCircle(lx, handY, hr, CHASSIS_SH);
  _t->drawPixel(lx - 1, handY - 1, CHASSIS_HI);

  // ── Right hand on deck B — rides the scratch so it jolts with the platter ──
  int rhx = rx + rNudge;
  _t->drawLine(rShoX,     shoulderY, rhx - 1, handY - 2, ARM);
  _t->drawLine(rShoX + 1, shoulderY, rhx,     handY - 2, ARM);
  _t->fillCircle(rhx, handY, hr, CHASSIS);
  _t->drawCircle(rhx, handY, hr, CHASSIS_SH);
  _t->drawPixel(rhx - 1, handY - 1, CHASSIS_HI);
}

// VU-meter visor — mirrors canvas DJVuVisor(levels[7]). Replaces the normal
// visor stripe with 7 vertical bars in club colors; bar heights are driven by
// `levels[i]` (0..10). Drawn at the canvas-absolute visor box (47,47 40×10),
// which sits over gr0m's visor stripe.
static void drawDJVuVisor(const uint8_t levels[7]) {
  const int baseX = 47, baseY = 47, w = 40, h = 10;
  // Visor frame (#2c2e30) + recessed black panel (#050505).
  _t->fillRect(baseX - 1, baseY - 1, w + 2, h + 2, 0x2966);
  _t->fillRect(baseX, baseY, w, h, 0x0020);
  // 7 bars. Canvas barW = (w-8)/7 ≈ 4.57; step = barW+0.5. Use a 5px pitch
  // (3px bar + gap) starting at baseX+3 so all 7 sit inside the panel.
  const int barW = 4;
  for (int i = 0; i < 7; i++) {
    int lvl = levels[i]; if (lvl > 10) lvl = 10; if (lvl < 0) lvl = 0;
    int bh = (lvl * (h - 2)) / 10; if (bh < 1) bh = 1;
    int bx = baseX + 3 + i * 5;
    _t->fillRect(bx, baseY + (h - 1) - bh, barW, bh, DJ_VU[i]);
  }
  // Scanline across the panel (canvas: faint white line at +5).
  _t->drawFastHLine(baseX + 2, baseY + 5, w - 4, 0x4228);
}

// Floating note glyphs — mirrors canvas DJNotes(count=8). 8 ♪/♫ glyphs at
// fixed canvas positions in 5 club colors. The 8x8 ROM font carries ♪ (\x0d)
// and ♫ (\x0e), so we use those as the note shapes. SIMPLIFICATION: the canvas
// rotates each glyph by (i-4)*6° — the ROM font can't rotate, so rotation is
// dropped and approximated with a 1px per-glyph slant offset; the larger
// (i%3==0) glyphs use text size 2, the rest size 1 (canvas 12px vs 9px).
static void drawDJNotes(uint32_t t) {
  static const int16_t NPOS[8][2] = {
    {14, 30}, {22, 18}, {110, 22}, {120, 38},
    {16, 70}, {118, 78}, {108, 56}, {10, 100},
  };
  // `seed` advances with the beat (canvas: Math.floor(beatPhase)) so the color
  // assignment cycles, reading as a live club. Tie it to the beat clock.
  int seed = (int)(t / DJ_BEAT_MS);
  for (int i = 0; i < 8; i++) {
    int x = NPOS[i][0], y = NPOS[i][1];
    int slant = (i - 4);                  // stand-in for the (i-4)*6° tilt
    _t->setTextColor(DJ_NOTE_COL[(i + seed) % 5], BUDDY_BG);
    _t->setTextSize((i % 3 == 0) ? 2 : 1);
    _t->setCursor(x + (slant > 0 ? 1 : 0), y + (slant & 1 ? 1 : 0));
    _t->print((i & 1) ? "\x0e" : "\x0d");   // ♫ / ♪ in the 8x8 ROM font
  }
  _t->setTextSize(1);
}

// DJ overlay composition — mirrors canvas DJMode/drawDJOverlay exactly.
// Draws the 4 overlay primitives (VU visor → turntable A → turntable B →
// mixer → notes) over whatever base bot has already been laid down. `t` is
// millis(); the 124-BPM beat clock (DJ_BEAT_MS) is the time base so this
// stays locked to the existing beat plumbing.
//   beat         = beats elapsed   (canvas `beatPhase`)
//   platterAngle = beat * 18       (deg)
//   cross        = sin(beat*0.4)*5 (px crossfader travel)
//   levels[i]    = min(10, base[i] + sin(beat*(0.6+i*0.1))*4)
// Small loudness floor (micLevel 0..255) below which the room counts as
// "silent" and we fall back to the synthetic 124-BPM animation.
static const uint8_t DJ_MIC_FLOOR = 10;

// One frame's worth of live-audio state, sampled ONCE per frame in djScene.
// Centralizing the sample matters because micBeat() is a CONSUMING one-shot:
// if both the head-bob path and drawDJOverlay called it independently they'd
// steal the onset from each other. djScene fills this and shares it.
//
// PRIVACY: this is a pure READ of the mic visualization feed (micSpectrum/
// micLevel/micBeat). It never registers a clap or touches the approval/relay
// path, and `live` is gated on the existing micEnabled() opt-in.
struct DJAudio {
  bool    live;      // mic opted-in AND room has signal above DJ_MIC_FLOOR
  uint8_t level;     // 0..255 smoothed room loudness (0 when not live)
  bool    beat;      // one-shot beat onset for THIS frame (already consumed)
};

static DJAudio djSampleAudio() {
  DJAudio a;
  a.live  = micEnabled() && micLevel() > DJ_MIC_FLOOR;
  a.level = a.live ? micLevel() : 0;
  a.beat  = a.live ? micBeat()  : false;   // consume the onset once, here
  return a;
}

static void drawDJOverlay(uint32_t t, const DJAudio& audio) {
  // Beats elapsed since boot — fractional, drives the synthetic canvas math.
  // Derived straight from the 124-BPM beat period so the FALLBACK visuals
  // stay phase-locked to djBeatEnvelope() (used for the head bob / antenna).
  float beat = (float)t / (float)DJ_BEAT_MS;

  static const uint8_t base_levels[7] = { 3, 5, 7, 4, 6, 3, 5 };
  uint8_t levels[7];

  // Data source already decided by djScene (sampled once — see DJAudio).
  bool    live    = audio.live;
  uint8_t lvl     = audio.level;     // 0..255 room loudness
  bool    beatHit = audio.beat;      // one-shot onset for this frame

  // Platter angle + deck-B scratch are PERSISTENT so audio events (beats)
  // can kick them and they ease back — they can't be a pure function of `t`
  // once the motion is event-driven. Initialized from the synthetic clock so
  // the first frame (and the fallback path) match the old behavior exactly.
  static float platterAngle = 0.0f;
  static float scratch      = 0.0f;
  static uint32_t lastT      = 0;
  uint32_t dt = (lastT == 0 || t < lastT) ? 16 : (t - lastT);
  lastT = t;
  if (dt > 100) dt = 100;   // clamp after a stall so we don't spin wildly

  int cross;

  if (live) {
    // ── LIVE: drive everything from the room audio ──────────────────
    // VU visor ← per-band Goertzel spectrum, low band left → high right.
    uint8_t spec[7];
    micSpectrum(spec);
    for (int i = 0; i < 7; i++) {
      int v = spec[i];
      if (v > 10) v = 10; if (v < 1) v = 1;   // keep a 1px floor like canvas
      levels[i] = (uint8_t)v;
    }

    // Platter spin speed scales with loudness: louder room ⇒ faster, peppier
    // spin; quiet ⇒ calm idle. ~6°/frame baseline up to ~30°/frame loud.
    float spin = (6.0f + (float)lvl * 0.09f) * ((float)dt / 16.0f);
    platterAngle += spin;

    // A beat SCRATCHES deck B and bumps the platter: kick a transient scratch
    // angle (sign alternates per beat) plus a small forward jump on the deck.
    static int8_t scratchSign = 1;
    if (beatHit) {
      scratch     += scratchSign * 16.0f;   // sharp deck-B scratch kick
      platterAngle += 10.0f;                // platter bump
      scratchSign  = -scratchSign;
    }
    // Scratch eases back toward 0 between beats (spring-like decay).
    scratch *= 0.80f;

    // Crossfader sweeps wider/faster when loud; a beat snaps it to one side.
    static float crossPhase = 0.0f;
    crossPhase += (0.04f + (float)lvl * 0.0006f) * ((float)dt / 16.0f);
    float crossAmp = 3.0f + (float)lvl * 0.012f;   // up to ~6px travel loud
    if (crossAmp > 6.0f) crossAmp = 6.0f;
    float crossF = sinf(crossPhase) * crossAmp;
    if (beatHit) crossF = (scratchSign > 0 ? crossAmp : -crossAmp);  // snap
    cross = (int)lroundf(crossF);
  } else {
    // ── FALLBACK: synthetic 124-BPM behavior (unchanged data math) ──
    platterAngle = beat * 18.0f;
    scratch      = sinf(beat * 0.7f) * 12.0f;          // canvas scratchAngle
    cross        = (int)lroundf(sinf(beat * 0.4f) * 5.0f);
    for (int i = 0; i < 7; i++) {
      int v = base_levels[i] + (int)lroundf(sinf(beat * (0.6f + i * 0.1f)) * 4.0f);
      if (v > 10) v = 10; if (v < 1) v = 1;
      levels[i] = (uint8_t)v;
    }
  }

  // Canvas DJMode draw order: notes are listed first in JSX (so they sit
  // behind the decks); the SPEC drawDJOverlay sample lists VU first. Match
  // the SPEC sample order: VU → A → B → mixer → notes. The 3D/draw pipeline
  // and composition are IDENTICAL — only the data above changed.
  drawDJVuVisor(levels);
  drawDJTurntable(22, 102, platterAngle, 'A');
  drawDJTurntable(113, 102, platterAngle + scratch, 'B');
  drawDJMixer(67, 102, cross);
  // Notes burst on a live beat (extra glyph energy), else the steady canvas
  // note field tied to the beat clock.
  drawDJNotes(t);
  if (live && beatHit) drawDJNotes(t + DJ_BEAT_MS / 2);   // second offset burst
}

// gr0mRenderDJ scene body — composed onto whatever surface _t points at.
// Called from the global trampoline below (which sets _t = tgt first).
// Lays down the base bot (head, headphones, hands, antenna) — the "base bot"
// the canvas DJMode composes over — then paints the canvas DJ overlay.
static void djScene(uint32_t t) {
  readTilt();   // keep the head parallax alive even in DJ mode

  // Sample the live-audio feed ONCE for this frame (consumes micBeat()).
  // Shared with drawDJOverlay so the beat onset isn't double-consumed.
  DJAudio audio = djSampleAudio();

  // Beat envelope (0..255) driving the head bob + antenna pulse. LIVE: a
  // beat onset kicks a fresh 255 envelope that we decay each frame off the
  // smoothed room loudness; quiet ⇒ small bob. FALLBACK: the synthetic
  // 124-BPM kick envelope (unchanged).
  uint8_t  beat;
  uint32_t beatNum;
  if (audio.live) {
    static uint8_t  liveEnv  = 0;     // decaying kick envelope
    static uint32_t liveBeatNum = 0;  // advances per detected beat (note color)
    if (audio.beat) { liveEnv = 255; liveBeatNum++; }
    else if (liveEnv > 16) liveEnv -= 16; else liveEnv = 0;
    // Floor the envelope to the room loudness so a sustained-loud room keeps
    // a steady bob even between discrete beats.
    uint8_t floorEnv = (uint8_t)((uint16_t)audio.level * 3 / 4);
    beat    = liveEnv > floorEnv ? liveEnv : floorEnv;
    beatNum = liveBeatNum;
  } else {
    uint32_t phaseMs = t % DJ_BEAT_MS;
    beat    = djBeatEnvelope(phaseMs);   // 0..255 synthetic kick envelope
    beatNum = t / DJ_BEAT_MS;
  }

  // Head bob: dip down hard on the kick, spring back between beats.
  _yProjOff = -(beat >> 6);   // 0..-3 px

  // ── Base bot: head + headphones + hands on the decks ────────────────
  // gr0m head, bobbing, with headphones on. The DJ overlay's VU visor is
  // drawn at canvas-absolute coords over the visor stripe afterward.
  drawHead3D();
  drawHeadphones();   // full-scale only (early-returns at scale 1)
  // Antenna LED pulses on the beat (live onset or the synthetic kick).
  drawAntenna3D(beat > 80 ? DJ_COLORS[beatNum % 6] : 0);
  // Hands resting on the deck platters (canvas DJTurntable centers 22/113,102);
  // right hand bobs with the kick.
  {
    int ly = 96, ry = 96 + (beat >> 6);
    _t->drawLine(HX - 16, HY + 30, 26, ly, CHASSIS_SH);
    _t->drawLine(HX + 16, HY + 30, 109, ry, CHASSIS_SH);
    _t->fillCircle(26, ly, 3, CHASSIS);  _t->drawCircle(26, ly, 3, CHASSIS_SH);
    _t->fillCircle(109, ry, 3, CHASSIS); _t->drawCircle(109, ry, 3, CHASSIS_SH);
  }

  // ── Canvas DJ overlay: VU visor, both decks, mixer, notes ────────────
  drawDJOverlay(t, audio);

  // (BPM readout retired — DJ mode no longer shows a "124"/BPM tag. The booth
  //  scene itself is no longer called; this body is kept only so the draw
  //  primitives above remain referenced. See gr0mRenderMusicEQ.)
}

// ════════════════════════════════════════════════════════════════════
//   MUSIC EQ VISUALIZER — replaces the gr0m character when DJ is on
// ────────────────────────────────────────────────────────────────────
//   A clean spectrum-analyzer that FILLS the character/buddy zone: a row
//   of vertical bars interpolated from the mic's 7-band Goertzel spectrum
//   (micSpectrum), club-color gradient by height, peak-hold caps, and
//   smooth rise / slow fall. When the room is quiet it idles with a gentle
//   travelling wave so the meter is never frozen. NO text, NO BPM, NO
//   turntables — just the EQ. Respects buddyScale() so the PET/INFO peek
//   shows a shrunk version in the upper character strip.
//
//   PRIVACY: pure READ of the mic visualization feed (micSpectrum). It
//   never registers a clap or touches the approval/relay path; when the
//   mic is disabled micSpectrum() returns all-zero and the idle wave runs.
// ════════════════════════════════════════════════════════════════════

// 16 bars interpolated up from the 7 spectrum bands — a dense, full-width,
// club-grade analyzer rather than the raw 7-band feed. 16 divides the 127px
// inner width cleanly and reads as the scene's bold centerpiece.
static const int EQ_BARS = 16;

// Per-bar smoothed height (0..1) and peak-hold cap (0..1), persisted across
// frames so they rise fast / fall slow and the caps hang then drift down.
static float _eqH[EQ_BARS]    = {0};
static float _eqPeak[EQ_BARS] = {0};

// Bar dynamics. Attack fast (snappy on a transient), release slow (musical
// decay). The cap hangs (slow drift) and re-snaps up whenever the bar passes it.
static const float EQ_RISE     = 0.55f;   // bar attack
static const float EQ_FALL     = 0.16f;   // bar release
static const float EQ_PEAK_FALL = 0.020f; // peak-hold cap drift-down per frame

// VERTICAL NEON gradient sampled by each pixel-row's HEIGHT within the meter:
// deep indigo at the floor → blue-violet → electric blue → cyan → magenta →
// hot-pink → near-white at the peaks. Index 0..(N-1) maps low→high so a tall
// bar climbs the whole neon ramp and lights up hot-pink/white at its tip.
static const uint16_t EQ_GRAD[12] = {
  0x40AF,  // lit indigo     rgb(70,20,120)  floor still reads off the glow
  0x4175,  // indigo-violet  rgb(70,45,170)
  0x3A9A,  // blue-violet    rgb(56,80,210)
  0x22DB,  // electric blue  rgb(32,90,220)
  0x14BD,  // blue-cyan      rgb(16,150,235)
  0x069D,  // cyan           rgb(0,210,235)
  0x7F9D,  // cyan-white     rgb(120,240,235)
  0xCBDD,  // magenta-cyan   rgb(200,120,235)
  0xF1F9,  // magenta        rgb(245,60,200)
  0xFA32,  // hot pink       rgb(255,70,150)
  0xFCB9,  // pink-white     rgb(255,150,200)
  0xFF1E,  // near white     rgb(255,225,245)
};
static const int EQ_GRAD_N = 12;
static uint16_t eqColorForHeight(float frac) {   // frac 0..1 (row pos in meter)
  if (frac < 0.f) frac = 0.f; if (frac > 1.f) frac = 1.f;
  int idx = (int)(frac * (EQ_GRAD_N - 1) + 0.5f);
  if (idx < 0) idx = 0; if (idx > EQ_GRAD_N - 1) idx = EQ_GRAD_N - 1;
  return EQ_GRAD[idx];
}

// drawMusicEQ — paint the equalizer into a given band on the active surface
// (_t already pointed by the caller). `t` is millis(); `baseY` is the bar
// floor and `topY` the ceiling (bars grow UP from baseY toward topY). The
// composed DJ scene (djMusicScene) passes a LOWER band so the EQ sits beneath
// the DJ character; the band stays clear of the y=0..20 HUD header and the
// stats footer drawHUD paints on top. Peak-hold state is per-bar and persists
// across frames so bars rise fast / fall slow and caps hang then drift down.
static void drawMusicEQ(uint32_t t, int baseY, int topY, int paneW = 0) {
  // Meter geometry. Bars sit on `baseY` and grow up toward `topY`. `paneW`>0
  // overrides the full BUDDY_CANVAS_W width (used for the compact landscape-clock
  // pane, which is only 110px wide); 0 keeps the original full-canvas behavior so
  // the HOME (y142) and peek (y64) callers render byte-for-byte as before.
  const int padX   = 4;
  const int canvasW = (paneW > 0) ? paneW : BUDDY_CANVAS_W;
  const int barAreaW = canvasW - padX * 2;            // full: 135-8=127, pane: 110-8=102
  const int fullH  = baseY - topY;                    // pixel span of a full bar

  // Live spectrum (0..10 per band, 7 bands). Zero when mic disabled/silent.
  uint8_t spec[7];
  micSpectrum(spec);
  int specSum = 0;
  for (int i = 0; i < 7; i++) specSum += spec[i];
  const bool quiet = (specSum == 0);

  // Per-bar target height in 0..1. Live: linear-interpolate the 7 bands up to
  // EQ_BARS and normalize 0..10 → 0..1. Quiet: a gentle travelling sine wave
  // so the meter idles low/calm but alive (never frozen).
  float target[EQ_BARS];
  if (quiet) {
    float phase = (float)t * 0.004f;                  // slow drift
    for (int b = 0; b < EQ_BARS; b++) {
      // Two summed waves → a gentle travelling swell across the meter so the
      // idle reads as a calm breathing centerpiece, not a flat sliver.
      float w = sinf(phase + b * 0.45f) * 0.5f + 0.5f;
      float w2 = sinf(phase * 0.6f - b * 0.18f) * 0.5f + 0.5f;
      target[b] = 0.12f + w * 0.18f + w2 * 0.08f;     // idle ~12%..38% height
    }
  } else {
    for (int b = 0; b < EQ_BARS; b++) {
      // Map bar b → fractional band position, lerp between adjacent bands.
      float fb = (float)b * (7 - 1) / (float)(EQ_BARS - 1);
      int   bi = (int)fb;
      float fr = fb - bi;
      int   lo = spec[bi];
      int   hi = spec[bi < 6 ? bi + 1 : 6];
      float v  = (lo * (1.0f - fr) + hi * fr) / 10.0f; // 0..1
      if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f;
      target[b] = v;
    }
  }

  // Smooth each bar (rise fast / fall slow) and update its peak-hold cap.
  for (int b = 0; b < EQ_BARS; b++) {
    if (target[b] > _eqH[b]) _eqH[b] += (target[b] - _eqH[b]) * EQ_RISE;
    else                     _eqH[b] += (target[b] - _eqH[b]) * EQ_FALL;
    if (_eqH[b] < 0.f) _eqH[b] = 0.f; if (_eqH[b] > 1.f) _eqH[b] = 1.f;
    if (_eqH[b] >= _eqPeak[b]) _eqPeak[b] = _eqH[b];          // snap cap up
    else { _eqPeak[b] -= EQ_PEAK_FALL; if (_eqPeak[b] < _eqH[b]) _eqPeak[b] = _eqH[b]; }
  }

  // Bar geometry. Distribute EQ_BARS evenly across the FULL bar area
  // (padX .. padX+barAreaW) so the meter fills the width and stays centered.
  // Old code truncated cellW = barAreaW/EQ_BARS (127/16 → 7), giving a 112px
  // meter pinned at padX with ~19px of dead space on the right — the EQ read
  // as left-shoved and short. Now each bar's left/right edge is derived from
  // barAreaW directly so the rounding spreads across all 16 bars instead of
  // piling into one gap; symmetric padX margins center it. A 1px right gap
  // keeps the neon tubes distinct. (Applies to full canvas AND the 110px
  // landscape-clock pane — both reduce to a padX-centered, full-area meter.)
  const int gap    = 1;
  const int x0     = padX;
  const int meterW = barAreaW;

  // The whole meter SWELLS on the kick — _djFlare adds a little extra height
  // and lifts the gradient sampling toward white so the EQ pulses with the
  // glow + head rim as one body. (0 outside the DJ scene → unchanged elsewhere.)
  const float swell  = 1.0f + (float)_djFlare * 0.0010f;   // up to +25% on a kick
  const int   gradLift = _djFlare / 64;                    // 0..3 LUT-steps up
                                                           // (lifts peaks hotter,
                                                           //  keeps the ramp)

  // Neon floor rail under the meter — a dim indigo baseline so the EQ reads as
  // grounded glowing tubing, brightening with the kick.
  uint16_t rail = EQ_GRAD[1];
  if (_djFlare) rail = rgb565add(rail, NEON_CYAN, _djFlare, 500);
  _t->drawFastHLine(x0, baseY + 1, meterW, rail);

  for (int b = 0; b < EQ_BARS; b++) {
    int bx  = padX + (b * barAreaW) / EQ_BARS;
    int bxn = padX + ((b + 1) * barAreaW) / EQ_BARS;
    int bw  = bxn - bx - gap; if (bw < 1) bw = 1;
    float h = _eqH[b] * swell; if (h > 1.0f) h = 1.0f;
    int bh = (int)(h * fullH + 0.5f);
    if (bh < 1) bh = 1;                                // 1px floor — always lit

    // Fill the bar bottom-up. Each pixel row is colored by ITS height fraction
    // (sampled from the vertical neon LUT) so the gradient runs lit-indigo at
    // the floor → cyan → magenta → hot-pink/white at the peak, independent of
    // how tall the bar is. The kick lifts the LUT index so bars flush hotter.
    // A 1px brightened LEFT edge makes each bar read as a lit neon tube that
    // pops off the violet stage glow rather than blending into it.
    for (int yy = 0; yy < bh; yy++) {
      int py = baseY - yy;
      float frac = (float)yy / (float)fullH;           // 0 at floor, →1 at ceiling
      int idx = (int)(frac * (EQ_GRAD_N - 1) + 0.5f) + gradLift;
      if (idx < 0) idx = 0; if (idx > EQ_GRAD_N - 1) idx = EQ_GRAD_N - 1;
      uint16_t col = EQ_GRAD[idx];
      _t->drawFastHLine(bx, py, bw, col);
      if (bw > 2) _t->drawPixel(bx, py, rgb565add(col, 0xFFFF, 60, 255));  // tube edge
    }

    // Glowing peak-hold cap: a bright near-white cap floating at the held peak,
    // with a dimmer 1px halo just under it so the cap reads as glowing, not a
    // hard line. The cap picks up the neon hue of its height + the kick flush.
    int pcap = (int)(_eqPeak[b] * swell * fullH + 0.5f);
    if (pcap > bh) {
      int cy = baseY - pcap; if (cy < topY) cy = topY;
      uint16_t capHue = eqColorForHeight((float)pcap / (float)fullH);
      uint16_t cap = rgb565add(capHue, 0xFFFF, 180 + _djFlare / 4, 255);  // bright
      uint16_t halo = rgb565add(capHue, 0x0000, 1, 1);                    // hue itself, dim
      _t->drawFastHLine(bx, cy, bw, cap);                 // bright cap
      if (cy + 1 < baseY) _t->drawFastHLine(bx, cy + 1, bw, halo);  // 1px halo under
    }
  }
}

// ════════════════════════════════════════════════════════════════════
//   DJ MUSIC SCENE — "late-night neon DJ booth", composed in the buddy zone
// ────────────────────────────────────────────────────────────────────
//   ONE cohesive, atmospheric "gr0m DJing the room" picture, drawn into the
//   SAME buddy zone buddyTick uses (full width, body band y≈18–148) so the HUD
//   header, stats footer, every screen + every menu still render normally on
//   top — this only swaps the CHARACTER layer, it does NOT take over the screen.
//   Nothing crosses y150.
//
//   FOUR layers, composed back→front, read as ONE neon scene:
//     • ATMOSPHERE (bg, y18–148): drawStageGlow — a near-black booth with a
//       soft violet "stage glow" pool rising from behind the decks, faked from
//       STEPPED GLOW_BAND fills + an ordered (Bayer) dither between steps (no
//       alpha). The pool FLARES brighter on the kick (saturating additive
//       blend off the shared _djFlare envelope) and settles between beats.
//     • HERO (~y22–76): the 3D gr0m head shrunk + raised into the top, wearing
//       HEADPHONES + SUNGLASSES + an easy DJ smile, NEON RIM-LIT (drawHeadRim:
//       a 1px cyan key edge + magenta fill edge that flushes white on the kick).
//       Nods to the room (kick head-bob + micLevel sustain), antenna LED
//       STROBES the neon palette on each beat. Parallax/3D lighting via readTilt.
//     • DECKS (RESTRAINED, ~y80–98): two slim spinning platters on a thin neon
//       mixer line, a compact mixer, and two hands resting — iconic, minimal,
//       NOT crammed. Platters SPIN (speed ∝ micLevel) + SCRATCH on micBeat.
//     • EQ CENTERPIECE (~y100–142): a bold full-width 16-bar spectrum with a
//       VERTICAL NEON gradient mapped by each row's height (deep-indigo floor →
//       blue → cyan → magenta → hot-pink/near-white peaks, sampled from a
//       stepped LUT), GLOWING peak-hold caps (bright cap + dimmer 1px halo),
//       smooth attack / slow release, and a kick swell. Calm travelling wave
//       when the room is silent — never frozen.
//
//   COHESION: glow + rim + antenna + mixer line + EQ all share the neon
//   cyan/magenta/hot-pink accent palette over near-black, and all swell off the
//   ONE shared _djFlare kick envelope, so the booth reads as a single body.
//
//   When the mic is opted-out / the room is quiet the whole scene still lives:
//   the glow breathes on the synthetic 124-BPM kick, the head holds a synthetic
//   nod, the platters keep an idle spin, and the EQ runs its calm travelling
//   wave. NO BPM readout, NO text. Everything is beat-reactive. The EQ floor
//   (y142) + its neon rail (y143) stay clear of the y=150 HUD footer drawHUD
//   paints on top, with a ~6px breathing gap so nothing crams or clips.
//
//   PRIVACY: pure READ of the mic visualization feed (micSpectrum/micLevel/
//   micBeat via djSampleAudio). It never registers a clap or touches the
//   approval/relay path; `live` is gated on the existing micEnabled() opt-in.
static void djMusicScene(uint32_t t) {
  const bool peek = (buddyScale() == 1);

  readTilt();   // keep the 3D head parallax + lighting alive while DJ-ing

  // Sample the live-audio feed ONCE per frame (consumes micBeat()). Shared
  // between the head-bob/antenna path and the deck spin/scratch below so the
  // one-shot beat onset isn't double-consumed.
  DJAudio audio = djSampleAudio();

  // Beat envelope (0..255) → head bob + antenna pulse. LIVE: a beat onset
  // snaps a fresh 255 kick that decays each frame, floored by the smoothed
  // room loudness so a sustained-loud room keeps a steady nod between
  // discrete onsets. QUIET/opted-out: the synthetic 124-BPM kick envelope so
  // the DJ never freezes. (Same plumbing as djScene's head bob.)
  uint8_t  beat;
  uint32_t beatNum;
  if (audio.live) {
    static uint8_t  liveEnv     = 0;
    static uint32_t liveBeatNum = 0;
    if (audio.beat) { liveEnv = 255; liveBeatNum++; }
    else if (liveEnv > 16) liveEnv -= 16; else liveEnv = 0;
    uint8_t floorEnv = (uint8_t)((uint16_t)audio.level * 3 / 4);
    beat    = liveEnv > floorEnv ? liveEnv : floorEnv;
    beatNum = liveBeatNum;
  } else {
    uint32_t phaseMs = t % DJ_BEAT_MS;
    beat    = djBeatEnvelope(phaseMs);
    beatNum = t / DJ_BEAT_MS;
  }

  // Head bob/nod: snap DOWN hard on the kick, spring back between beats. A
  // small sustained component (folded into `beat` via floorEnv when live)
  // keeps a loud room riding. Smaller throw than the booth scene because the
  // head is shrunk into its band. 0..-2 px home / 0..-1 px peek.
  _yProjOff = peek ? -(beat >> 7) : -(beat >> 7);

  // ── Kick FLARE envelope (shared atmosphere driver) ──────────────────
  // The glow, head rim and EQ all swell off ONE decaying envelope so the scene
  // pulses as a single body. Snap toward `beat` (the onset/loudness), then ease
  // down ~12%/frame so the flare flares on the kick and settles between beats.
  if (beat > _djFlare) _djFlare = beat;
  else { int f = _djFlare - _djFlare / 7 - 6; _djFlare = (uint8_t)(f < 0 ? 0 : f); }

  // ── Stage atmosphere (background): near-black booth + violet glow pool ──
  // Filled FIRST so everything composes over it. Brightens on the kick.
  if (!peek) drawStageGlow(18, 148);

  // ── gr0m the DJ (HERO, ~y22–76) ─────────────────────────────────────
  // Shrink + raise the 3D head into the top strip via the DJ head transform
  // (see _djHeadScale/_djHeadCenterY): scale 0.70 about HY, recentered on y≈50,
  // so the head + headphones tuck into the hero band and the ANTENNA TIP clears
  // the y≈20 HUD header even with the beat head-bob lifting it. Head bottom
  // (ear cups) lands ≈y72, leaving the lower zone for the decks + EQ. Peek
  // keeps the existing mini projection.
  if (!peek) { _djHeadScale = 0.70f; _djHeadCenterY = 50; }
  drawHead3D();                       // 3D chassis head, lit + parallax
  drawVisor3D(VISOR_IDLE);            // cyan visor stripe (shows under the shades)
  drawSunglasses3D(VISOR_IDLE);       // DJ shades over the visor (Stage 3+)
  drawMouth3D(2);                     // easy DJ smile
  drawHeadphones();                   // arched cans + ear-cups (rides the transform)
  drawHeadRim();                      // 1px cyan/magenta neon rim, flushes on kick
  // Antenna LED STROBES through the neon palette on each beat; dark between
  // beats so the pulse reads as "lit by the drop".
  drawAntenna3D(beat > 80 ? NEON_STROBE[beatNum & 3] : 0);
  _djHeadScale = 1.0f; _djHeadCenterY = HY;   // reset — nothing below is head-relative

  // Peek (PET/INFO mini): no room for the full booth — keep the head + a short
  // EQ strip tucked above the y=70 panel, skipping the decks band entirely.
  if (peek) {
    drawMusicEQ(t, 64, 50);
    return;
  }

  // ── Decks (RESTRAINED, ~y80–98) ─────────────────────────────────────
  // Two slim spinning platters + a thin mixer line + hands resting — iconic,
  // minimal, NOT crammed. Persistent deck state: platters SPIN continuously
  // (speed ∝ micLevel) and SCRATCH/jolt on a beat (alternating-sign kick that
  // springs back). State is static because the motion is event-driven — it
  // can't be a pure f(t) once a beat can kick it. The fallback path seeds it
  // from the 124-BPM clock so a quiet room still spins.
  static float platterAngle = 0.0f;   // deg, deck A base spin
  static float scratch      = 0.0f;   // deg, deck B transient scratch (springs back)
  static int8_t scratchSign = 1;
  static uint32_t lastT = 0;
  uint32_t dt = (lastT == 0 || t < lastT) ? 16 : (t - lastT);
  lastT = t;
  if (dt > 100) dt = 100;             // clamp after a stall so we don't spin wildly

  int cross;
  if (audio.live) {
    // Spin speed scales with loudness: ~6°/frame idle → ~30°/frame loud.
    float spin = (6.0f + (float)audio.level * 0.09f) * ((float)dt / 16.0f);
    platterAngle += spin;
    if (audio.beat) {
      scratch     += scratchSign * 16.0f;   // sharp deck-B scratch kick
      platterAngle += 10.0f;                // platter bump on the kick
      scratchSign  = -scratchSign;
    }
    scratch *= 0.80f;                        // spring back toward 0 between beats
    // Crossfader sweeps wider/faster when loud; a beat snaps it to one side.
    static float crossPhase = 0.0f;
    crossPhase += (0.04f + (float)audio.level * 0.0006f) * ((float)dt / 16.0f);
    float crossAmp = 3.0f + (float)audio.level * 0.012f; if (crossAmp > 6.0f) crossAmp = 6.0f;
    float crossF = sinf(crossPhase) * crossAmp;
    if (audio.beat) crossF = (scratchSign > 0 ? crossAmp : -crossAmp);
    cross = (int)lroundf(crossF);
  } else {
    float fbeat  = (float)t / (float)DJ_BEAT_MS;
    platterAngle = fbeat * 18.0f;            // synthetic continuous spin
    scratch      = sinf(fbeat * 0.7f) * 12.0f;
    cross        = (int)lroundf(sinf(fbeat * 0.4f) * 5.0f);
  }

  // Deck geometry: two platters (r=11) flanking a SLIM mixer, sized to the
  // 135px width. Centers at x=22 / x=113 put the platters near the edges with
  // the mixer at x=67. Platter row at y≈87 sits just under the hero head and
  // above the EQ centerpiece; hands rest a touch above the platter centers and
  // the forearms tuck up toward the (smaller, higher) head band (shoulderY≈70).
  const int deckY  = 87;
  const int deckAx = 22, deckBx = 113;
  // A thin neon mixer line tying the two decks together — the booth bench. Drawn
  // under the platters so they sit ON it; brightens a touch on the kick.
  uint16_t bench = EQ_GRAD[2];
  if (_djFlare) bench = rgb565add(bench, NEON_MAGENTA, _djFlare, 600);
  _t->drawFastHLine(deckAx, deckY + 12, deckBx - deckAx, bench);
  drawDJTurntable(deckAx, deckY, platterAngle,           'A');
  drawDJTurntable(deckBx, deckY, platterAngle + scratch, 'B');
  drawDJMixer(HX, deckY, cross);
  // Hands reaching down onto the decks — the right hand rides deck B's scratch.
  drawDJHands(deckAx, deckBx, deckY - 13, 70, (int)lroundf(scratch * 0.25f));

  // ── EQ centerpiece (~y100–142) — the memorable element ──────────────
  // A bold full-width 16-bar spectrum with a VERTICAL NEON gradient mapped by
  // each row's height (deep-indigo floor → cyan → magenta → hot-pink/white
  // peaks), glowing peak-hold caps, smooth attack / slow release, and a kick
  // swell. The floor (y142) + its neon rail (y143) stay clear of the y=150 HUD
  // footer (RUN/WAIT/TOK strip, STRIP_Y=150) drawHUD paints on top — a ~6px
  // gap (y144..149) so when BUSY the EQ rail and the footer's top border line
  // don't merge into one crammed band (the device photo showed them touching).
  // Calm travelling wave when the room is silent — never frozen.
  drawMusicEQ(t, 142, 100);
}

// ════════════════════════════════════════════════════════════════════
//   COMPACT DJ SCENE — for the LANDSCAPE-CLOCK left pane (petSpr 110x135)
// ────────────────────────────────────────────────────────────────────
//   The home djMusicScene is composed for the full 135px portrait canvas
//   (head + decks + a 127px-wide EQ) and won't fit the narrow 110px landscape
//   clock pane. This is the trimmed sibling the user asked for: a small neon
//   rim-lit gr0m head up top (~y8–60) + a compact, pane-width neon EQ below it
//   (~y68–128) over the dark stage backdrop — no decks, no hands. The clock
//   text lives on the right (x=180) on the LCD, clear of this pane.
//
//   Runs at peek scale (the landscape clock renders at 1× — see buddyRenderTo),
//   so the existing peek head projection (ANCHOR_Y=32, FOCAL=50) already shrinks
//   the head into the top strip. We only nudge it left (_xProjOff) so it centers
//   in the 110px pane (pane center 55, vs the head's native HX=67) and retarget
//   the stage glow + EQ width to the pane.
//
//   Same neon vocabulary as home: EQ_GRAD gradient, peak-hold caps, micSpectrum/
//   micLevel/micBeat reactivity via djSampleAudio, and the shared _djFlare kick
//   envelope so the glow + head rim + EQ all swell together. PRIVACY identical:
//   pure READ of the mic visualization feed; never registers a clap.
static void djMusicSceneClock(uint32_t t, int paneW, int paneH) {
  // Pane center (110 → 55). The head's native screen-x is HX=67; nudge it left
  // so it sits centered in the narrow pane via the projection's _xProjOff.
  const int paneCx = paneW / 2;

  readTilt();   // keep the 3D head parallax + lighting alive while DJ-ing

  // Sample the live-audio feed ONCE per frame (consumes micBeat()), exactly as
  // djMusicScene does, so the compact scene reacts identically to the room.
  DJAudio audio = djSampleAudio();

  uint8_t  beat;
  uint32_t beatNum;
  if (audio.live) {
    static uint8_t  liveEnv     = 0;
    static uint32_t liveBeatNum = 0;
    if (audio.beat) { liveEnv = 255; liveBeatNum++; }
    else if (liveEnv > 16) liveEnv -= 16; else liveEnv = 0;
    uint8_t floorEnv = (uint8_t)((uint16_t)audio.level * 3 / 4);
    beat    = liveEnv > floorEnv ? liveEnv : floorEnv;
    beatNum = liveBeatNum;
  } else {
    uint32_t phaseMs = t % DJ_BEAT_MS;
    beat    = djBeatEnvelope(phaseMs);
    beatNum = t / DJ_BEAT_MS;
  }

  // Head bob (peek throw, 0..-1 px) off the kick.
  _yProjOff = -(beat >> 7);
  // Shift the peek head to the pane center: native head-x is HX, want paneCx.
  _xProjOff = paneCx - HX;

  // Shared kick FLARE envelope — same swell driver as home so the glow, head
  // rim and EQ pulse as one body.
  if (beat > _djFlare) _djFlare = beat;
  else { int f = _djFlare - _djFlare / 7 - 6; _djFlare = (uint8_t)(f < 0 ? 0 : f); }

  // ── Stage atmosphere: dark booth + violet glow pool, retargeted to the pane
  //    (centered under x=paneCx, clamped to the pane width). Fills the whole
  //    pane so head + EQ compose over it.
  drawStageGlow(0, paneH, paneW, paneCx);

  // ── gr0m the DJ head (HERO, ~y8–60) ─────────────────────────────────
  // Peek projection already lands the head in the top strip (anchor y=32). The
  // neon rim + strobing antenna match the home hero exactly.
  drawHead3D();                       // 3D chassis head, lit + parallax
  drawVisor3D(VISOR_IDLE);            // cyan visor stripe (shows under the shades)
  drawSunglasses3D(VISOR_IDLE);       // DJ shades over the visor (Stage 3+)
  drawMouth3D(2);                     // easy DJ smile
  drawHeadphones();                   // arched cans + ear-cups (rides the transform)
  drawHeadRim();                      // 1px cyan/magenta neon rim, flushes on kick
  drawAntenna3D(beat > 80 ? NEON_STROBE[beatNum & 3] : 0);
  _xProjOff = 0;                      // reset — EQ below is not head-relative

  // ── Compact EQ (~y68–128) — pane-width neon spectrum ────────────────
  // Same 16-bar gradient/peak-cap/beat-reactive meter as home, sized to the
  // pane (paneW) so it fills the width cleanly without overflowing 110px. Floor
  // at y128 (clear of the y135 pane bottom, leaving room for the neon rail).
  drawMusicEQ(t, 128, 68, paneW);
}

// ── States ──────────────────────────────────────────────────────────

static void doSleep(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = ((t / 6) & 1) ? 1 : 0;
  drawShadow(0);
  drawChest3D();
  drawBolt3D(VISOR_OFF);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_OFF);
  drawMouth3D(1);                   // closed flat
  drawAntenna3D(0);                 // LED off
  if (evoStage() >= 5) {            // Stage 5 (Ascended): sleep gear unlocks
    drawNightcap();                 // drooping sleep cap (costume gear)
    drawZParticles(t);              // Zzz drifting up
  }
  // No mood motes in sleep — at the 5fps gated redraw the rising pink motes
  // popped/stepped and read as a flicker beside the bot. Motes now appear ONLY
  // in the transient emotional one-shots (celebrate/dizzy/heart). Stage 5 still
  // shows Zzz here.
}

static void doIdle(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawRide(t);  // #tilt-skate/surf/hover board (drawn first, under the bot) —
                // automatic Stage-5 idle flourish (no longer a user toggle)
  drawShadow(0);
  // Pixel desk + coffee mug under the bot — only paints in landscape clock
  // mode (drawDesk() self-gates on the render surface), matching the canvas
  // "AT HIS DESK" scene that pairs the desk with the sideways clock. Drawn
  // before the body so the bot sits in front of it.
  drawDesk();
  drawChest3D();
  // Chest mark. The token LCD readout is NO LONGER swapped in here on a timer —
  // that bolt↔LCD alternation read as a flicker. The green-digital LCD is now
  // the signature of ONE outfit only (the "digital" costume, which sets
  // _forceChestLcd); every other look wears the lightning bolt, steady.
  if (_forceChestLcd) {
    char buf[8];
    uint32_t v = ::g_dispTokens;   // period figure mirrored from main.cpp
    if (v >= 1000000)   snprintf(buf, sizeof(buf), "%luM", v / 1000000);
    else if (v >= 1000) snprintf(buf, sizeof(buf), "%luK", v / 1000);
    else                snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
    drawChestLCD(buf);
  } else {
    drawBolt3D(VISOR_IDLE);
  }
  // #tilt-bobble — head spring (idle, |tilt|>10°) → _xProjOff for the draws below.
  applyHeadBobble(fabsf(_tiltDegX) > 10.0f);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_IDLE);
  // Sunglasses are the idle persona from Stage 3 up — including Stage 5, where
  // the FULL-GEAR ROBOT (shades + cyan visor) is the everyday look. The human
  // disguise is a SEPARATE easter-egg state (A+B toggle → gr0mRenderHumanCostume),
  // never an overlay on the normal moods.
  drawSunglasses3D();
  drawMouth3D(((t / 25) % 5 == 0) ? 2 : 0);
  drawAntenna3D(0);
  // No mood motes in idle (the everyday view) — the rising pink motes flickered
  // at the 5fps redraw. The bot still feels alive via the blink + tilt/bobble.
}

static void doBusy(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawShadow(0);
  bool ledPulse = (t / 2) & 1;
  drawChest3D();
  drawBolt3D(ledPulse ? VISOR_BUSY : VISOR_OFF);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_BUSY);   // sunglasses OFF in busy — the bare green visor shows
  drawMouth3D(3);                   // grimace
  drawAntenna3D(ledPulse ? VISOR_BUSY : 0);
  // Stage 5 (Ascended): the busy WORK RIG unlocks — headphones (over the head),
  // a tiny pixel laptop with a glowing green code screen, and jointed hands on
  // the keys. This is the full-gear ROBOT per the design; the human disguise is
  // a SEPARATE easter-egg state, NOT an overlay here. Below Stage 5 the busy
  // mood still renders (green visor + grimace), just bare.
  if (evoStage() >= 5) {
    drawHeadphones();   // full-scale only (early-returns at scale 1)
    drawLaptop(t);
    drawHandsAtLaptop();
  }
  // No mood motes in busy — flickered at the 5fps redraw (see doSleep note).
}

static void doAttention(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawShadow(0);
  bool pulse = (t / 2) & 1;
  drawChest3D();
  drawBolt3D(pulse ? VISOR_ALERT : VISOR_OFF);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_ALERT);   // RED alert visor, sunglasses OFF so the alarm reads
  drawMouth3D(4);                     // O shout
  drawAntenna3D(pulse ? VISOR_ALERT : 0);
  // No mood motes in attention — a prompt can wait a while, and the flickering
  // pink motes beside the alert added clutter (see doSleep note).
  // Stage 5 (Ascended): the alert speech bubble unlocks, pointing at the viewer.
  // It CYCLES between a bold "!" and "BASH?!" (per the design carousel) so both
  // reads appear over time. Text is INK (black) on white — far more legible at
  // the 6px ROM-font scale than alert-red, which washes out against the white
  // bubble. Per the canvas (gr0m.jsx Gr0m) the bubble gates on stage >= 5; below
  // that the attention mood still renders (red visor + "!" mouth), bare. The
  // human disguise is a SEPARATE easter-egg state and is NOT drawn here.
  if (evoStage() >= 5) {
    const char* alert = ((t / 6) & 1) ? "BASH?!" : "!";
    drawSpeechBubble(alert, INK);
  }
}

static void doCelebrate(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  // Jump bounce — vertical projection offset
  _yProjOff = ((t / 2) & 1) ? -4 : -7;
  drawShadow(0);
  static const uint16_t RAINBOW[] = { 0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x05FF, 0xF81F };
  drawChest3D();
  drawBolt3D(RAINBOW[(t + 1) % 6]);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(RAINBOW[(t + 3) % 6]);
  drawSunglasses3D(RAINBOW[(t + 3) % 6]);   // cyan-band slivers cycle with the visor
  drawMouth3D(2);                     // smile
  drawAntenna3D(RAINBOW[t % 6]);
  if (evoStage() >= 5) {              // Stage 5 (Ascended): party gear unlocks
    drawPartyHat();                   // pointy hat with stripes + pom
    drawConfetti(t);                  // confetti rain across the screen
  }
  drawMoodParticles(t, 6, 1);
}

static void doDizzy(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawShadow(0);
  uint16_t v = (t & 1) ? VISOR_OFF : VISOR_IDLE;
  drawChest3D();
  drawBolt3D(v);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(v);
  // Dizzy: sunglasses OFF (the off-kilter eyes + X mouth need to read), antenna
  // blinks crimson. The human disguise is a SEPARATE easter-egg state, not here.
  drawMouth3D(6);                     // X
  drawAntenna3D((t & 3) == 0 ? CRIMSON : 0);
  drawMoodParticles(t, 3, 2);
}

static void doHeart(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawShadow(0);
  drawChest3D();
  drawBolt3D(HEART_RED);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_LOVE);   // MAGENTA love visor
  // Heart: sunglasses ON (cool robot in love), blush mouth, floating heart cloud.
  // The human disguise is a SEPARATE easter-egg state, not an overlay here.
  drawSunglasses3D(VISOR_LOVE);   // magenta visor shows between + under the lenses
  drawMouth3D(((t / 8) & 1) ? 2 : 7);
  drawAntenna3D(((t / 3) & 1) ? HEART_RED : 0);
  if (evoStage() >= 5) drawHeartCloud(t);     // Stage 5 (Ascended): heart-cloud gear
  drawMoodParticles(t, 4, 2);
}

// ── Showcase outfit dispatcher (Settings ▸ cycle) ────────────────────
// Renders one finished full look on demand — NOT the token-driven evolution
// scaffolding. Robot/ride outfits temporarily force the evo mirror to a full
// loadout (4 = HUD gear) or Stage 5 (rides), restoring it after. Index order
// matches OUTFIT_NAMES in main.cpp:
//   0 robot (full-gear, bolt)  1 DJ booth  2 costume undercover
//   3 costume blond  4 skate  5 surf  6 hover  7 green digital (LCD chest)
static void renderOutfit(uint32_t t, uint8_t outfit) {
  uint8_t savedEvo = ::g_evoStage;
  switch (outfit) {
    case 1: djMusicScene(t); break;                              // DJ booth
    case 2: _costumeVar = &CV_UNDERCOVER; humanCostumeScene(t); break;
    case 3: _costumeVar = &CV_BLOND;      humanCostumeScene(t); break;
    case 4: case 5: case 6:                                      // rides: robot + board
      ::g_evoStage = 5;
      _forceRide = outfit - 3;                                   // 1 skate / 2 surf / 3 hover
      doIdle(t);
      _forceRide = 0;
      break;
    case 7:                                                      // green digital — LCD chest
      ::g_evoStage = 4;                                          // the only look with the LCD
      _forceChestLcd = true;
      doIdle(t);
      _forceChestLcd = false;
      break;
    default:                                                     // 0 — full-gear robot (bolt)
      ::g_evoStage = 4;                                          // HUD loadout, no costume/ride
      doIdle(t);
      break;
  }
  ::g_evoStage = savedEvo;
}

}  // namespace gr0m

// DJ-booth scene entry point — global so main.cpp's djTick can call it
// without touching the gr0m namespace internals. Points the namespace's
// active render surface at `tgt` (mirrors how each state function does
// `_t = buddyTarget()`), then composes the scene. `t` is millis().
void gr0mRenderDJ(TFT_eSPI* tgt, uint32_t t) {
  gr0m::_t = tgt;
  gr0m::djScene(t);
}

// DJ-mode entry point — global so main.cpp can render it in place of the gr0m
// CHARACTER when DJ mode is on (mirrors how buddyTick renders the character
// into the buddy zone). Points the namespace's active surface at `tgt` first
// (like each state function's `_t = buddyTarget()`), then composes the DJ
// scene: the improved gr0m DJ character (head + headphones + sunglasses,
// beat-reactive bob + antenna pulse) up top, with the mic-reactive EQ beneath
// it. `t` is millis(). The HUD chrome / screens / menus render normally on
// top — this only swaps the character layer.
//
// Name kept as gr0mRenderMusicEQ so the main.cpp integration is unchanged
// (the djActive render site still calls this exact symbol).
void gr0mRenderMusicEQ(TFT_eSPI* tgt, uint32_t t) {
  gr0m::_t = tgt;
  gr0m::djMusicScene(t);
}

// COMPACT DJ-mode entry point for the LANDSCAPE-CLOCK left pane. main.cpp's
// drawClockLandscape calls this (instead of buddyRenderTo) into the 110x135
// petSpr when djActive, so the clock shows a small rim-lit gr0m head + a
// pane-width neon EQ on the left with the time on the right — rather than the
// normal pet character. `w`/`h` are the pane dimensions (110x135). The caller
// owns clearing/pushing the sprite, matching buddyRenderTo's contract. The
// landscape clock always renders at peek scale (1×), which the compact scene
// relies on for the small head projection — so we force it here regardless of
// the prior scale, then restore it (mirrors buddyRenderTo).
void gr0mRenderMusicEQClock(TFT_eSPI* tgt, uint32_t t, int w, int h) {
  gr0m::_t = tgt;
  uint8_t prevScale = buddyScale();
  buddySetPeek(true);                 // landscape clock pane renders at 1×
  gr0m::djMusicSceneClock(t, w, h);
  buddySetPeek(prevScale == 1);       // restore the caller's scale (1=peek, 2=home)
}

// Human-costume manual-toggle entry point (the A+B full-screen scene) —
// global so the main loop can call it without touching the gr0m namespace
// internals. Points the namespace's active render surface at `tgt` (mirrors
// gr0mRenderDJ), then composes the full canvas costume scene. `t` is millis().
// Wired to the A+B button combo in main.cpp; renders the same six-element
// disguise as the evoStage>=5 auto-render.
void gr0mRenderHumanCostume(TFT_eSPI* tgt, uint32_t t) {
  gr0m::_t = tgt;
  gr0m::_costumeVar = &gr0m::CV_UNDERCOVER;   // A+B easter egg = canonical undercover
  gr0m::humanCostumeScene(t);
}

// Showcase outfit entry point (Settings ▸ cycle) — renders any one of the
// finished looks into the buddy zone: 0 robot / 1 DJ / 2 costume-undercover /
// 3 costume-blond / 4 skate / 5 surf / 6 hover / 7 green-digital. `t` = millis().
void gr0mRenderOutfit(TFT_eSPI* tgt, uint32_t t, uint8_t outfit) {
  gr0m::_t = tgt;
  gr0m::renderOutfit(t, outfit);
}
uint8_t gr0mOutfitCount() { return 8; }

extern const Species GR0M_SPECIES = {
  "gr0m",
  0xF800,
  { gr0m::doSleep, gr0m::doIdle, gr0m::doBusy, gr0m::doAttention,
    gr0m::doCelebrate, gr0m::doDizzy, gr0m::doHeart }
};
