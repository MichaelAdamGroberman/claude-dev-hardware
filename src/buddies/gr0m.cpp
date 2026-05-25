#include "../buddy.h"
#include "../buddy_common.h"
#include "../stats.h"     // stats().tokens — for the chest-LCD token counter
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
enum ItemState : uint8_t { ITEM_ATTACHED = 0, ITEM_FALLING = 1, ITEM_SETTLED = 2 };
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
  return { (int)(HX + _xProjOff + v.x * FOCAL / z),
           (int)(ANCHOR_Y + _yProjOff + v.y * FOCAL / z) };
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
static const uint16_t VISOR_IDLE = 0x05FF;  // cyan
static const uint16_t VISOR_BUSY = 0x07E0;  // green (focused mode)
static const uint16_t VISOR_ALERT= 0xF800;  // red
static const uint16_t VISOR_LOVE = 0xF81F;  // magenta
static const uint16_t VISOR_OFF  = 0x18C3;  // dim slate

// ── Stage-5 "human disguise" palette ────────────────────────────────
// The robot's unconvincing attempt to pass as a person: a tan trench
// coat, a darker fedora, and opaque human glasses. Warm browns so the
// disguise reads as fabric/felt against the cold brushed-steel chassis.
static const uint16_t COAT_TAN   = 0xB425;  // trench-coat khaki
static const uint16_t COAT_SH    = 0x8302;  // coat shadow / fold lines
static const uint16_t COAT_HI    = 0xD568;  // coat highlight / seam
static const uint16_t HAT_BROWN  = 0x4163;  // fedora felt
static const uint16_t HAT_SH     = 0x2061;  // fedora shadow
static const uint16_t HAT_BAND   = 0x18E3;  // fedora hatband (near-black)

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

// 3D sunglasses — lens centers projected onto the rotated face (POSITIONS
// track rotation; circles don't squash to ellipses).
static void drawSunglasses3D() {
  if (evoStage() < 3) return;            // Stage 3 (Persona): shades on
  if (!frontFaceVisible()) return;
  // #tilt-gravity detached: slide off down-side + drop (rot omitted).
  float sx = 0, sy = 0;
  if (_shadeState == ITEM_FALLING) {
    float s = fabsf(_tiltDegX) - 35.0f; if (s > 25.0f) s = 25.0f;
    sx = s * (_tiltDegX > 0 ? 1 : -1);
    sy = fabsf(sx) * 0.6f;
  }
  V2 lL = onFace(-12 + sx, -1 + sy);
  V2 lR = onFace( 12 + sx, -1 + sy);
  V2 bL = onFace(-4  + sx, -1 + sy);
  V2 bR = onFace( 4  + sx, -1 + sy);
  int lr = pkS(8); if (lr < 2) lr = 2;   // lens radius — scaled so shades fit the mini face
  // Lens fills
  _t->fillCircle(lL.x, lL.y, lr, INK);
  _t->fillCircle(lR.x, lR.y, lr, INK);
  // Frames
  _t->drawCircle(lL.x, lL.y, lr, STEEL);
  _t->drawCircle(lL.x, lL.y, lr + 1, STEEL);
  _t->drawCircle(lR.x, lR.y, lr, STEEL);
  _t->drawCircle(lR.x, lR.y, lr + 1, STEEL);
  // Nose bridge — projected line between lens edges
  _t->drawLine(bL.x, bL.y, bR.x, bR.y, STEEL);
  _t->drawLine(bL.x, bL.y - 1, bR.x, bR.y - 1, STEEL);
  // Specular highlights
  V2 hL = onFace(-15 + sx, -5 + sy);
  V2 hR = onFace( 10 + sx, -5 + sy);
  _t->drawPixel(hL.x, hL.y, SPECULAR);
  _t->drawPixel(hL.x + 1, hL.y, SPECULAR);
  _t->drawPixel(hR.x, hR.y, SPECULAR);
  _t->drawPixel(hR.x + 1, hR.y, SPECULAR);
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
  const int FADE = LIFECYCLE / 2;
  int baseX = -32;
  for (int i = 0; i < n; i++) {
    int phase = ((int)t * 2 / speed + i * 5) % LIFECYCLE;
    int y = HY + 38 - phase;
    if (y < 2 || y > HY + 40) continue;
    int x = HX + baseX + (n > 1 ? (i * 22) / (n - 1) : 0);
    x += ((phase / 2) & 1) ? 1 : -1;
    // Drift with gravity — particles ride a consistent world-frame "up"
    // axis so they all feel like a coherent atmosphere.
    x += (-_gravX * phase) / 8;
    if (phase < FADE) {
      drawHeart(pkX(x), pkY(y), HEART_RED);
    } else {
      drawSparkle(pkX(x), pkY(y), SPECULAR);
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
  int endY = HY - HH/2 + fy + 6;     // y=38 — just below the head-top edge
  for (int i = -23; i <= 23; i++) {
    float n = (float)i / 23.0f;
    int yOff = (int)((1.0f - n * n) * 7.0f);   // crest 7 px over the ends
    int y = endY - yOff;
    _t->drawPixel(cx + i, y,     STEEL);
    _t->drawPixel(cx + i, y + 1, STEEL);
    _t->drawPixel(cx + i, y,     0x52AA);       // inner highlight stroke
  }
  // Ear cups — sit over the head sides at y=44 (canvas x=38 / x=90, 6×10).
  // Fixed (don't rotate with face) so they read like real over-ears.
  int eY = HY - HH/2 + fy + 12;      // y=44
  _t->fillRect(cx - 29, eY,     6, 10, STEEL);   // left cup  (x=38)
  _t->fillRect(cx + 23, eY,     6, 10, STEEL);   // right cup (x=90)
  // Cup shadow seam on the left can (canvas Px 38,45,1,8)
  _t->fillRect(cx - 29, eY + 1, 1, 8, 0x52AA);
  // Red foam ring stripe (canvas: x=43 left / x=90 right, y=47, 1×4)
  _t->fillRect(cx - 24, eY + 3, 1, 4, CRIMSON);
  _t->fillRect(cx + 23, eY + 3, 1, 4, CRIMSON);
  // Subtle highlight specks on the outer cup corners
  _t->drawPixel(cx - 29, eY,     SPECULAR);
  _t->drawPixel(cx + 28, eY,     SPECULAR);
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
  // existing parallax tilt, not the later #tilt-gravity detach behavior.
  int sway = _tiltX; if (sway < -12) sway = -12; if (sway > 12) sway = 12;
  int px = hx + pkS(18) + pkS(sway), py = hy - pkS(22) + pkS(abs(_tiltX) / 5);
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
static uint8_t _rideMode = 0;
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

static void drawRide() {   // home-scale only
  if (_rideMode == 0 || buddyScale() != 2) return;
  if      (_rideMode == 1) drawSkateboard();
  else if (_rideMode == 2) drawSurfboard();
  else                     drawHoverboard();
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
//   STAGE-5 FINAL FORM — "robot in a trench coat" human disguise
// ────────────────────────────────────────────────────────────────────
//   At the final evolution stage gr0m tries to pass as a person to hide
//   that it's an AGI: a long trench coat with an upturned collar over the
//   chest, a fedora on the head, opaque human-style glasses, and an
//   occasional "tell" (a flat speech bubble insisting it's normal). It's
//   deliberately unconvincing — a steel cube cosplaying a human.
//
//   Geometry follows the existing 3D helpers:
//     • the coat rides the CHEST front plane (z = +12, like drawBolt3D)
//       so it tracks tilt with the body
//     • the fedora + glasses ride the HEAD front plane (z = +22, like
//       drawSunglasses3D / onFace) and only draw when the front face is
//       visible — they vanish as the head turns away, same as the visor
// ════════════════════════════════════════════════════════════════════

// Project a point on the CHEST front-face plane (z = +12) to screen. Mirror
// of onFace() but for the body, so coat panels track the chest the way the
// brand bolt does in drawBolt3D.
static V2 onChest(float x, float y) {
  return rp({ x, y, 12.0f });
}

// Long trench coat draped over the chest/body, with an upturned collar that
// rises toward the neck and a center seam with buttons. Rides the chest
// front plane so it leans with body tilt. No backface gate — the coat wraps
// the whole torso, so it stays sensible even as the body turns.
static void drawTrenchcoat() {
  // Coat body — a broad panel over the chest front, slightly wider than the
  // chest cube so it reads as draped fabric rather than painted-on.
  V2 tl = onChest(-24, 27);    // shoulders (chest top is y≈27)
  V2 tr = onChest( 24, 27);
  V2 br = onChest( 20, 50);    // hem, tucked in a touch at the bottom
  V2 bl = onChest(-20, 50);
  _t->fillTriangle(tl.x, tl.y, tr.x, tr.y, br.x, br.y, COAT_TAN);
  _t->fillTriangle(tl.x, tl.y, br.x, br.y, bl.x, bl.y, COAT_TAN);
  // Lapels — two angled flaps opening from the collar down to mid-chest,
  // drawn in shadow so the V of the opening reads.
  V2 nkL = onChest(-5, 27);
  V2 nkR = onChest( 5, 27);
  V2 lpL = onChest(-22, 30);
  V2 lpR = onChest( 22, 30);
  V2 mid = onChest( 0, 40);
  _t->fillTriangle(nkL.x, nkL.y, lpL.x, lpL.y, mid.x, mid.y, COAT_SH);
  _t->fillTriangle(nkR.x, nkR.y, lpR.x, lpR.y, mid.x, mid.y, COAT_SH);
  // Center seam + buttons down the front.
  V2 sTop = onChest(0, 30);
  V2 sBot = onChest(0, 49);
  _t->drawLine(sTop.x, sTop.y, sBot.x, sBot.y, COAT_SH);
  for (int by = 36; by <= 46; by += 5) {
    V2 b = onChest(1, (float)by);
    _t->fillCircle(b.x, b.y, 1, HAT_BAND);
  }
  // Upturned collar — two short panels standing up either side of the neck,
  // the classic "hiding my face" trench-coat collar. Rises above the chest
  // top toward the head.
  V2 cL0 = onChest(-17, 27);
  V2 cL1 = onChest(-2, 7);     // huge collar peak rising up around the face
  V2 cL2 = onChest(-11, 27);
  V2 cR0 = onChest( 17, 27);
  V2 cR1 = onChest( 2, 7);
  V2 cR2 = onChest( 11, 27);
  _t->fillTriangle(cL0.x, cL0.y, cL1.x, cL1.y, cL2.x, cL2.y, COAT_HI);
  _t->fillTriangle(cR0.x, cR0.y, cR1.x, cR1.y, cR2.x, cR2.y, COAT_HI);
  _t->drawLine(cL0.x, cL0.y, cL1.x, cL1.y, COAT_SH);
  _t->drawLine(cR0.x, cR0.y, cR1.x, cR1.y, COAT_SH);
  // Shoulder seam highlight.
  _t->drawLine(tl.x, tl.y, tr.x, tr.y, COAT_HI);
}

// Fedora perched on the head — felt crown + wide brim with a dark hatband.
// Rides the head front plane (z = +22) so it tracks rotation, and only
// draws when the front face is visible like the other face-mounted gear.
static void drawFedora() {
  if (!frontFaceVisible()) return;
  // Brim — a flat-ish ellipse straddling the top of the head (head top
  // y≈-20). Built from two triangles across the front plane so it skews
  // with rotation. Drawn first so the crown overlaps it.
  V2 bL = onFace(-32, -11);
  V2 bR = onFace( 32, -11);
  V2 bF = onFace(  0,  -6);    // front lip dips low toward the viewer
  V2 bB = onFace(  0, -16);    // back edge
  _t->fillTriangle(bL.x, bL.y, bR.x, bR.y, bF.x, bF.y, HAT_BROWN);
  _t->fillTriangle(bL.x, bL.y, bR.x, bR.y, bB.x, bB.y, HAT_BROWN);
  _t->drawLine(bL.x, bL.y, bF.x, bF.y, HAT_SH);
  _t->drawLine(bR.x, bR.y, bF.x, bF.y, HAT_SH);
  // Tall crown — trapezoid sitting on the brim, jammed down low, pinched on top.
  V2 kBL = onFace(-19, -12);
  V2 kBR = onFace( 19, -12);
  V2 kTL = onFace(-14, -36);
  V2 kTR = onFace( 14, -36);
  _t->fillTriangle(kBL.x, kBL.y, kBR.x, kBR.y, kTR.x, kTR.y, HAT_BROWN);
  _t->fillTriangle(kBL.x, kBL.y, kTR.x, kTR.y, kTL.x, kTL.y, HAT_BROWN);
  // Pinch dent — a darker crease down the crown center.
  V2 dT = onFace(0, -36);
  V2 dB = onFace(0, -14);
  _t->drawLine(dT.x, dT.y, dB.x, dB.y, HAT_SH);
  // Hatband — dark stripe around the base of the crown.
  V2 hbL = onFace(-19, -13);
  V2 hbR = onFace( 19, -13);
  _t->drawLine(hbL.x, hbL.y, hbR.x, hbR.y, HAT_BAND);
  _t->drawLine(hbL.x, hbL.y + 1, hbR.x, hbR.y + 1, HAT_BAND);
  // Crown top highlight.
  _t->drawLine(kTL.x, kTL.y, kTR.x, kTR.y, COAT_HI);
}

// Opaque human-style glasses — squarer than the aviator sunglasses, with a
// heavy "trying too hard to look studious" frame. Rides the head front
// plane and only draws when the front face is visible. When wired into a
// Stage-5 everyday state this REPLACES the bot's own sunglasses (the
// disguise is the headline final look, so it wins the conflict).
static void drawHumanGlasses() {
  if (!frontFaceVisible()) return;
  // Big round "thick studious nerd" lenses sitting over the visor band. Round
  // (not square) reads more comically + separates cleanly from the straight
  // mustache below. Sized in raw px so they shrink on the mini via pkS().
  V2 cL = onFace(-10, -4);   // left lens center (raised so the 'stache clears)
  V2 cR = onFace( 10, -4);   // right lens center
  int r = pkS(8); if (r < 3) r = 3;
  // White "googly" lens fill so the eyes read as a disguise, not the bot's
  // own black shades — the over-eager human look. Dark heavy frame around.
  _t->fillCircle(cL.x, cL.y, r, SPECULAR);
  _t->fillCircle(cR.x, cR.y, r, SPECULAR);
  _t->drawCircle(cL.x, cL.y, r,     HAT_BAND);
  _t->drawCircle(cL.x, cL.y, r + 1, HAT_BAND);
  _t->drawCircle(cR.x, cR.y, r,     HAT_BAND);
  _t->drawCircle(cR.x, cR.y, r + 1, HAT_BAND);
  // Tiny dark "pupils" behind the lenses, parallax-shifted, so the disguise
  // has shifty little eyes peering through.
  int pr = pkS(2); if (pr < 1) pr = 1;
  V2 pL = onFace(-10 + (float)_tiltX * 0.6f, -3);
  V2 pR = onFace( 10 + (float)_tiltX * 0.6f, -3);
  _t->fillCircle(pL.x, pL.y, pr, INK);
  _t->fillCircle(pR.x, pR.y, pr, INK);
  // Heavy nose bridge between the lenses.
  V2 brL = onFace(-2, -4);
  V2 brR = onFace( 2, -4);
  _t->drawLine(brL.x, brL.y, brR.x, brR.y, HAT_BAND);
  _t->drawLine(brL.x, brL.y + 1, brR.x, brR.y + 1, HAT_BAND);
  // Temple arms running back toward the ears.
  V2 tL = onFace(-24, -6);
  V2 tR = onFace( 24, -6);
  _t->drawLine(cL.x - r, cL.y - 1, tL.x, tL.y, HAT_BAND);
  _t->drawLine(cR.x + r, cR.y - 1, tR.x, tR.y, HAT_BAND);
  // Specular glints so the lenses read as glass, parallax-shifted.
  V2 gL = onFace(-13 + (float)_tiltX, -7);
  V2 gR = onFace(  7 + (float)_tiltX, -7);
  _t->drawPixel(gL.x, gL.y, 0x52AA);
  _t->drawPixel(gR.x, gR.y, 0x52AA);
}

// The "tell" — gr0m periodically over-acts being human with a flat speech
// bubble ("HELLO HUMAN" / "I AM NORMAL"), and otherwise wears a stiff fake
// smile so the disguise reads as unconvincing. The bubble cycles every few
// seconds; between bubbles only the smile shows. Reuses drawSpeechBubble.
static void drawHumanTell(uint32_t t) {
  // ~2.6 s per phase at 5 fps; show a bubble ~40% of the time, then a beat
  // of nothing so the over-acting feels intermittent rather than constant.
  uint8_t phase = (t / 13) % 5;
  if (phase == 0) {
    drawSpeechBubble("HELLO HUMAN", INK);
  } else if (phase == 2) {
    drawSpeechBubble("I AM NORMAL", INK);
  }
  // Stiff fake smile BELOW the mustache (mustache bottom ≈ y13) — a too-wide
  // flat grin with corners hitched up, on the front face. Only when the face
  // is toward us, like the other face decorations.
  if (!frontFaceVisible()) return;
  V2 sL = onFace(-8, 17);
  V2 sR = onFace( 8, 17);
  V2 cL = onFace(-9, 15);    // corners hitched up — forced smile
  V2 cR = onFace( 9, 15);
  _t->drawLine(sL.x, sL.y, sR.x, sR.y, CHASSIS_SH);
  _t->drawLine(sL.x, sL.y - 1, sR.x, sR.y - 1, CHASSIS_SH);
  _t->drawLine(sL.x, sL.y, cL.x, cL.y, CHASSIS_SH);
  _t->drawLine(sR.x, sR.y, cR.x, cR.y, CHASSIS_SH);
}

// Comically oversized fake mustache — the disguise's loudest "tell": a fat
// black Groucho bush sitting clearly BELOW the glasses (with a chassis gap so
// it doesn't merge into the lenses), swooping up into curled tips at both
// ends, split by a notch under the nose so the two halves read as a 'stache.
static void drawFakeMustache() {
  if (!frontFaceVisible()) return;
  // Main bush — a wide, fat slab. Top at y=8 (clear of the lens bottoms ~y=4,
  // lenses centered at y=-4 with r=8), bottom at y=13. Two halves + a notch.
  // Left half.
  V2 lOut = onFace(-16, 8);    // outer-top, where the tip will curl up from
  V2 lTop = onFace( -2, 9);    // inner-top (near the notch)
  V2 lBot = onFace( -3, 13);   // inner-bottom
  V2 lLow = onFace(-13, 13);   // outer-bottom
  _t->fillTriangle(lOut.x, lOut.y, lTop.x, lTop.y, lBot.x, lBot.y, INK);
  _t->fillTriangle(lOut.x, lOut.y, lBot.x, lBot.y, lLow.x, lLow.y, INK);
  // Right half (mirror).
  V2 rOut = onFace( 16, 8);
  V2 rTop = onFace(  2, 9);
  V2 rBot = onFace(  3, 13);
  V2 rLow = onFace( 13, 13);
  _t->fillTriangle(rOut.x, rOut.y, rTop.x, rTop.y, rBot.x, rBot.y, INK);
  _t->fillTriangle(rOut.x, rOut.y, rBot.x, rBot.y, rLow.x, rLow.y, INK);
  // Curled-up handlebar tips sweeping above the bush at both ends.
  V2 lTip = onFace(-21, 4), lj = onFace(-15, 6);
  V2 rTip = onFace( 21, 4), rj = onFace( 15, 6);
  _t->fillTriangle(lOut.x, lOut.y, lj.x, lj.y, lTip.x, lTip.y, INK);
  _t->fillTriangle(rOut.x, rOut.y, rj.x, rj.y, rTip.x, rTip.y, INK);
}

// One-call composite for the Stage-5 disguise everyday look. Order matters:
// coat first (body layer), then the head gear over the chassis, then the
// mustache + tell on top of everything.
static void drawHumanDisguise(uint32_t t) {
  drawTrenchcoat();
  drawFedora();
  drawHumanGlasses();
  drawFakeMustache();
  drawHumanTell(t);
}

// ════════════════════════════════════════════════════════════════════
//   ALT HUMAN COSTUME — "undercover" easter egg (NOT the trench coat)
// ────────────────────────────────────────────────────────────────────
//   A second, deliberately worse human disguise, distinct from the
//   Stage-5 trench-coat-and-fedora look. Here gr0m pulls on a brown wig,
//   straps a flesh-tone face mask (with painted-on human eyes + nose)
//   over the visor, throws a knit hoodie over the chest plate, and clutches
//   a takeaway coffee cup to "look busy / normal". It's bad on PURPOSE:
//   the square robot head corners and chassis rivets still poke out past
//   the mask + wig, and the mouth twitches between an awkward straight line
//   and a too-eager smile every ~2 s.
//
//   Geometry follows the existing 3D helpers so it tracks tilt like the
//   rest of the character:
//     • the wig + flesh mask + human eyes/nose/mouth ride the HEAD front
//       plane (z = +22, via onFace) — they only render when the front face
//       is toward us, so they vanish as the head turns away just like the
//       visor and sunglasses do
//     • the knit hoodie + V-neck + drawstrings ride the CHEST front plane
//       (z = +12, via onChest) like the trench coat / brand bolt
//     • the coffee cup is a held prop anchored beside the chest via rp()
//       so it leans with the body
//   Exposed below the namespace as gr0mRenderHumanCostume() — a standalone
//   full-screen scene like the DJ booth, not an evolution-gated mood.
// ════════════════════════════════════════════════════════════════════

// Costume palette (RGB565). Browns/flesh tones so the disguise reads as
// fabric + skin against the cold brushed-steel chassis underneath.
static const uint16_t HC_SKIN     = 0xED90;  // #e8b386 flesh
static const uint16_t HC_SKIN_SH  = 0xBC2B;  // #b8825a cheek / nose shadow
static const uint16_t HC_HAIR     = 0x59C3;  // #5a3a1e warm brown wig (reads clearly as hair)
static const uint16_t HC_HAIR_HI  = 0x8B0C;  // #8a5a30 lighter strand highlight
static const uint16_t HC_HOODIE   = 0x39ED;  // #3a4a6b knit hoodie
static const uint16_t HC_HOODIE_DK= 0x29CB;  // #2a3858 pocket pouch
static const uint16_t HC_HOODIE_LI= 0x5B51;  // #5a6a8b seam highlight / stitches
static const uint16_t HC_HOODIE_TR= 0x1907;  // #1a2238 trim / hem
static const uint16_t HC_STRING   = 0xCE59;  // drawstring cord
static const uint16_t HC_MOUTH    = 0x79C3;  // #7a3818 painted mouth
static const uint16_t HC_CUP       = 0xFFFF; // white takeaway cup
static const uint16_t HC_CUP_LID   = 0x39C3; // brown lid / rim
static const uint16_t HC_CUP_SLV   = 0xCCCD; // kraft sleeve
static const uint16_t HC_STEAM     = 0xDEFB; // pale steam wisp

// Knit hoodie over the chest plate, with a hood draped behind the head, a
// V-neck of skin showing, drawstrings, a kangaroo pocket, and sparse knit
// stitches. Rides the chest front plane (onChest) so it leans with the body
// like the trench coat. The hood "wings" use rp() so they splay either side
// of the head and track tilt too.
static void drawCostumeHoodie() {
  // ── Hood drape behind the head ── two soft wings rising from the
  // shoulders up past the head sides. Drawn first so the head + wig overlap.
  // Built on the chest plane so they lean with the torso.
  V2 hwL0 = onChest(-22, 26);   // left shoulder
  V2 hwL1 = onChest(-30, 10);   // up the left of the head
  V2 hwL2 = onChest(-14, 22);   // inner fold
  V2 hwR0 = onChest( 22, 26);
  V2 hwR1 = onChest( 30, 10);
  V2 hwR2 = onChest( 14, 22);
  _t->fillTriangle(hwL0.x, hwL0.y, hwL1.x, hwL1.y, hwL2.x, hwL2.y, HC_HOODIE_DK);
  _t->fillTriangle(hwR0.x, hwR0.y, hwR1.x, hwR1.y, hwR2.x, hwR2.y, HC_HOODIE_DK);

  // ── Main body — broad knit panel over the chest front, a touch wider
  // than the chest cube so it reads as draped fabric.
  V2 tl = onChest(-24, 26);
  V2 tr = onChest( 24, 26);
  V2 br = onChest( 22, 52);
  V2 bl = onChest(-22, 52);
  _t->fillTriangle(tl.x, tl.y, tr.x, tr.y, br.x, br.y, HC_HOODIE);
  _t->fillTriangle(tl.x, tl.y, br.x, br.y, bl.x, bl.y, HC_HOODIE);
  // Collar seam highlight + hem trim.
  _t->drawLine(tl.x, tl.y, tr.x, tr.y, HC_HOODIE_LI);
  _t->drawLine(bl.x, bl.y, br.x, br.y, HC_HOODIE_TR);

  // ── V-neck — triangle of skin showing at the collar.
  V2 vL = onChest(-11, 26);
  V2 vR = onChest( 11, 26);
  V2 vB = onChest(  0, 36);
  _t->fillTriangle(vL.x, vL.y, vR.x, vR.y, vB.x, vB.y, HC_SKIN);
  _t->drawLine(vL.x, vL.y, vB.x, vB.y, HC_HOODIE_TR);
  _t->drawLine(vR.x, vR.y, vB.x, vB.y, HC_HOODIE_TR);

  // ── Drawstrings dangling from the collar.
  V2 dsL0 = onChest(-5, 36), dsL1 = onChest(-6, 44);
  V2 dsR0 = onChest( 4, 36), dsR1 = onChest( 5, 43);
  _t->drawLine(dsL0.x, dsL0.y, dsL1.x, dsL1.y, HC_STRING);
  _t->drawLine(dsR0.x, dsR0.y, dsR1.x, dsR1.y, HC_STRING);
  _t->fillCircle(dsL1.x, dsL1.y, pkS(1), HC_HOODIE_LI);
  _t->fillCircle(dsR1.x, dsR1.y, pkS(1), HC_HOODIE_LI);

  // ── Kangaroo pocket pouch across the lower front.
  V2 pTL = onChest(-15, 44), pTR = onChest( 15, 44);
  V2 pBR = onChest( 13, 50), pBL = onChest(-13, 50);
  _t->fillTriangle(pTL.x, pTL.y, pTR.x, pTR.y, pBR.x, pBR.y, HC_HOODIE_DK);
  _t->fillTriangle(pTL.x, pTL.y, pBR.x, pBR.y, pBL.x, pBL.y, HC_HOODIE_DK);
  _t->drawLine(pTL.x, pTL.y, pTR.x, pTR.y, HC_HOODIE_TR);

  // ── Sparse knit stitches — short diagonals across the body.
  for (int i = 0; i < 5; i++) {
    float x = -16.0f + i * 8.0f;
    V2 a = onChest(x,        38);
    V2 b = onChest(x + 3.0f, 42);
    _t->drawLine(a.x, a.y, b.x, b.y, HC_HOODIE_LI);
  }
}

// Flesh-tone face mask strapped over the visor: a skin slab with painted-on
// human eyes (whites + brown pupils that shift a touch with tilt), a nose
// shadow, brow lines, cheek shadows, and a mouth whose shape is the twitch
// channel. Rides the head front plane (onFace) and only draws front-on.
// `expr`: 1 = eager smile, 2 = awkward flat line (the ~2 s twitch pair).
static void drawCostumeFaceMask(uint8_t expr) {
  if (!frontFaceVisible()) return;
  // Skin slab over the visor / lower face. Deliberately a hair too small for
  // the square head, so the chassis corners + rivets still poke out — that's
  // the joke. Two triangles so it skews with rotation.
  V2 mTL = onFace(-19, -10);
  V2 mTR = onFace( 19, -10);
  V2 mBR = onFace( 17,  17);
  V2 mBL = onFace(-17,  17);
  _t->fillTriangle(mTL.x, mTL.y, mTR.x, mTR.y, mBR.x, mBR.y, HC_SKIN);
  _t->fillTriangle(mTL.x, mTL.y, mBR.x, mBR.y, mBL.x, mBL.y, HC_SKIN);

  // Cheek shadows for a little dimension.
  V2 chL = onFace(-13, 8), chR = onFace(11, 8);
  _t->fillCircle(chL.x, chL.y, pkS(2), HC_SKIN_SH);
  _t->fillCircle(chR.x, chR.y, pkS(2), HC_SKIN_SH);

  // Brow lines above each eye.
  V2 brL0 = onFace(-16, -6), brL1 = onFace(-5, -6);
  V2 brR0 = onFace(  5, -6), brR1 = onFace(16, -6);
  _t->drawLine(brL0.x, brL0.y, brL1.x, brL1.y, HC_SKIN_SH);
  _t->drawLine(brR0.x, brR0.y, brR1.x, brR1.y, HC_SKIN_SH);

  // Human eyes — white sclera blocks with brown pupils that drift slightly
  // with tilt (shifty disguise eyes), a dark outline, and a catchlight pixel.
  float px = (float)_tiltX * 0.5f;
  V2 eL = onFace(-10, -1), eR = onFace(10, -1);
  int ew = pkS(6), eh = pkS(5); if (ew < 3) ew = 3; if (eh < 3) eh = 3;
  _t->fillRect(eL.x - ew, eL.y - eh / 2, ew * 2, eh, HC_CUP);
  _t->fillRect(eR.x - ew, eR.y - eh / 2, ew * 2, eh, HC_CUP);
  _t->drawRect(eL.x - ew, eL.y - eh / 2, ew * 2, eh, HC_SKIN_SH);
  _t->drawRect(eR.x - ew, eR.y - eh / 2, ew * 2, eh, HC_SKIN_SH);
  V2 pL = onFace(-10 + px, -1), pR = onFace(10 + px, -1);
  int pr = pkS(2); if (pr < 1) pr = 1;
  _t->fillCircle(pL.x, pL.y, pr, HC_HAIR);
  _t->fillCircle(pR.x, pR.y, pr, HC_HAIR);
  _t->drawPixel(pL.x - 1, pL.y - 1, HC_CUP);
  _t->drawPixel(pR.x - 1, pR.y - 1, HC_CUP);

  // Nose — a thin vertical shadow + a little nostril shelf.
  V2 nT = onFace(0, 2), nB = onFace(0, 7);
  _t->drawLine(nT.x, nT.y, nB.x, nB.y, HC_SKIN_SH);
  V2 nL = onFace(-2, 7), nR = onFace(2, 7);
  _t->drawLine(nL.x, nL.y, nR.x, nR.y, HC_SKIN_SH);

  // Mouth — the twitch channel. 1 = eager smile arc, else awkward flat line.
  if (expr == 1) {
    V2 sL = onFace(-8, 11), sC = onFace(0, 15), sR = onFace(8, 11);
    _t->drawLine(sL.x, sL.y, sC.x, sC.y, HC_MOUTH);
    _t->drawLine(sC.x, sC.y, sR.x, sR.y, HC_MOUTH);
    _t->drawLine(sL.x, sL.y + 1, sC.x, sC.y + 1, HC_MOUTH);
    _t->drawLine(sC.x, sC.y + 1, sR.x, sR.y + 1, HC_MOUTH);
  } else {
    V2 fL = onFace(-8, 12), fR = onFace(8, 12);
    _t->drawLine(fL.x, fL.y, fR.x, fR.y, HC_MOUTH);
    _t->drawLine(fL.x, fL.y + 1, fR.x, fR.y + 1, HC_MOUTH);
    // tiny lopsided tic so the awkward beat reads as forced
    V2 tic = onFace(-5, 13);
    _t->drawPixel(tic.x, tic.y, HC_MOUTH);
    _t->drawPixel(tic.x, tic.y + 1, HC_MOUTH);
  }

  // Stubble shadow specks along the chin.
  for (int i = -1; i <= 1; i++) {
    V2 s = onFace(i * 5.0f, 16);
    _t->drawPixel(s.x, s.y, HC_SKIN_SH);
  }
}

// Brown wig wrapping the top + sides of the head. Rides the head front
// plane so it tracks rotation; draws front-on only. Sits a little above the
// chassis top so the square head edge still shows beneath the hairline.
static void drawCostumeWig() {
  if (!frontFaceVisible()) return;
  // Main wig mass — a broad band across the forehead. Sits a touch inside the
  // square chassis top (y=-20) so the head's corners + rivets still show.
  V2 wTL = onFace(-21, -20);
  V2 wTR = onFace( 21, -20);
  V2 wBR = onFace( 22,  -6);   // fringe drops down to the brow on the right
  V2 wBL = onFace(-22,  -6);
  _t->fillTriangle(wTL.x, wTL.y, wTR.x, wTR.y, wBR.x, wBR.y, HC_HAIR);
  _t->fillTriangle(wTL.x, wTL.y, wBR.x, wBR.y, wBL.x, wBL.y, HC_HAIR);
  // Rounded crown — a fat dome above the band so it reads as a head of hair,
  // not a cap. Two triangles fanning up to a peaked top.
  V2 cTL = onFace(-15, -30);
  V2 cTR = onFace( 15, -30);
  V2 cPk = onFace(  0, -33);
  _t->fillTriangle(wTL.x, wTL.y, wTR.x, wTR.y, cTR.x, cTR.y, HC_HAIR);
  _t->fillTriangle(wTL.x, wTL.y, cTR.x, cTR.y, cTL.x, cTL.y, HC_HAIR);
  _t->fillTriangle(cTL.x, cTL.y, cTR.x, cTR.y, cPk.x, cPk.y, HC_HAIR);
  // Sideburns down the temples (a few px thick).
  V2 sbL0 = onFace(-21, -6), sbL1 = onFace(-20, 4);
  V2 sbR0 = onFace( 21, -6), sbR1 = onFace( 20, 4);
  for (int o = 0; o < 3; o++) {
    _t->drawLine(sbL0.x + o, sbL0.y, sbL1.x + o, sbL1.y, HC_HAIR);
    _t->drawLine(sbR0.x - o, sbR0.y, sbR1.x - o, sbR1.y, HC_HAIR);
  }
  // Hairline fringe wisps — a jagged bottom edge so it isn't a flat block.
  for (int x = -16; x <= 16; x += 5) {
    V2 a = onFace((float)x, -6);
    V2 b = onFace((float)x + 2, -2);
    _t->drawLine(a.x, a.y, b.x, b.y, HC_HAIR);
  }
  // A side part + a couple of stray highlight strands on the crown.
  V2 pt0 = onFace(-4, -28), pt1 = onFace(-6, -12);
  _t->drawLine(pt0.x, pt0.y, pt1.x, pt1.y, HC_HAIR_HI);
  V2 hi0 = onFace(6, -26), hi1 = onFace(12, -20);
  _t->drawLine(hi0.x, hi0.y, hi1.x, hi1.y, HC_HAIR_HI);
}

// Takeaway coffee cup held beside the chest — completes the "just a normal
// commuter" look. Anchored on the chest plane via onChest so it leans with
// the body; a little steam wisps off the lid.
static void drawCostumeCoffee(uint32_t t) {
  // Cup body — a tapered white tube to the lower right of the chest.
  V2 cTL = onChest(26, 36), cTR = onChest(36, 36);
  V2 cBR = onChest(34, 52), cBL = onChest(28, 52);
  _t->fillTriangle(cTL.x, cTL.y, cTR.x, cTR.y, cBR.x, cBR.y, HC_CUP);
  _t->fillTriangle(cTL.x, cTL.y, cBR.x, cBR.y, cBL.x, cBL.y, HC_CUP);
  // Brown rim under the lid.
  _t->drawLine(cTL.x, cTL.y, cTR.x, cTR.y, HC_CUP_LID);
  _t->drawLine(cTL.x, cTL.y + 1, cTR.x, cTR.y + 1, HC_CUP_LID);
  // Lid tab above the rim.
  V2 lL = onChest(28, 33), lR = onChest(34, 33);
  _t->drawLine(lL.x, lL.y, lR.x, lR.y, HC_CUP_LID);
  V2 ltL = onChest(30, 31), ltR = onChest(32, 31);
  _t->drawLine(ltL.x, ltL.y, ltR.x, ltR.y, HC_CUP_LID);
  // Kraft sleeve band around the middle.
  V2 svTL = onChest(26, 43), svTR = onChest(35, 43);
  V2 svBR = onChest(34, 47), svBL = onChest(27, 47);
  _t->fillTriangle(svTL.x, svTL.y, svTR.x, svTR.y, svBR.x, svBR.y, HC_CUP_SLV);
  _t->fillTriangle(svTL.x, svTL.y, svBR.x, svBR.y, svBL.x, svBL.y, HC_CUP_SLV);
  _t->drawLine(svTL.x, svTL.y, svTR.x, svTR.y, HC_CUP_LID);
  // Steam — three short wisps rising + drifting (animated, anti-gravity).
  for (int i = 0; i < 3; i++) {
    int phase = ((int)(t / 120) + i * 4) % 12;
    V2 s = onChest(28.0f + i * 3.0f, 28.0f - phase);
    if (s.y < 1) continue;
    _t->drawPixel(s.x, s.y, HC_STEAM);
    _t->drawPixel(s.x + ((phase & 1) ? 1 : -1), s.y - 1, HC_STEAM);
  }
}

// One-call composite for the alt costume. Order matters: hoodie (body layer)
// behind, then the face mask over the visor, then the wig over the chassis
// top, then the held coffee cup on top of everything.
static void drawAltHumanCostume(uint32_t t, uint8_t expr) {
  drawCostumeHoodie();
  drawCostumeFaceMask(expr);
  drawCostumeWig();
  drawCostumeCoffee(t);
}

// Alt-costume scene body — composed onto whatever surface _t points at.
// Called from the global trampoline below (which sets _t = tgt first).
// Draws the base bot first (so the square head corners + rivets peek out
// past the disguise), then layers the costume. The mouth twitches between
// awkward (2) and smile (1) every ~2 s; `t` is millis().
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
  drawVisor3D(VISOR_IDLE);   // visor under the mask — peeks at the head edges
  drawAntenna3D(0);
  // Costume on top. ~2 s twitch: 2000 ms per half-cycle.
  uint8_t expr = ((t / 2000) & 1) ? 1 : 2;   // 1 smile, 2 awkward
  drawAltHumanCostume(t, expr);
  // A faint mood wisp for atmosphere, like the other scenes.
  drawMoodParticles(t, 2, 2);
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

// One turntable: platter rim, vinyl grooves, center label, and a rotating
// radial mark whose angle is supplied by the caller (steady spin on the
// left deck, beat-driven jitter on the right). cx/cy = platter center.
static void drawTurntable(int cx, int cy, float angle) {
  const int R = 16;
  // Platter body + rim
  _t->fillCircle(cx, cy, R, 0x2104);
  _t->drawCircle(cx, cy, R, STEEL);
  _t->drawCircle(cx, cy, R - 1, CHASSIS_SH);
  // Vinyl grooves
  _t->drawCircle(cx, cy, 12, 0x4208);
  _t->drawCircle(cx, cy, 9,  0x39E7);
  // Center label (gr0m red) + spindle
  _t->fillCircle(cx, cy, 5, CRIMSON);
  _t->drawCircle(cx, cy, 5, SPECULAR);
  _t->fillCircle(cx, cy, 1, INK);
  // Rotating radial mark — tells you it's spinning.
  int mx = cx + (int)(cosf(angle) * (R - 2));
  int my = cy + (int)(sinf(angle) * (R - 2));
  _t->drawLine(cx, cy, mx, my, SPECULAR);
  // Tonearm stub anchored at the upper-right of the platter.
  _t->drawLine(cx + R, cy - R, cx + 3, cy - 3, CHASSIS_HI);
  _t->fillCircle(cx + R, cy - R, 2, STEEL);
}

// The mixer between the two decks: two short channel faders, a crossfader
// that slides L↔R on the beat, and a pair of EQ knobs. `slide` is 0..255
// (the beat envelope) and positions the crossfader cap.
static void drawMixer(int cx, int cy, uint8_t slide) {
  const int mw = 34, mh = 30;
  int mx = cx - mw / 2, my = cy - mh / 2;
  _t->fillRoundRect(mx, my, mw, mh, 3, 0x18C3);
  _t->drawRoundRect(mx, my, mw, mh, 3, CHASSIS_SH);
  // Two channel faders (vertical slots) with caps near the top.
  for (int i = 0; i < 2; i++) {
    int fx = mx + 7 + i * 14;
    _t->drawFastVLine(fx, my + 4, 12, CHASSIS_SH);
    int cap = my + 5 + ((i == 0) ? (slide >> 6) : (3 - (slide >> 6)));
    _t->fillRect(fx - 2, cap, 5, 2, DJ_COLORS[i ? 4 : 3]);
  }
  // EQ knobs.
  _t->fillCircle(mx + 7, my + 22, 2, STEEL);
  _t->fillCircle(mx + 21, my + 22, 2, STEEL);
  _t->drawPixel(mx + 7, my + 21, SPECULAR);
  // Crossfader — horizontal slot near the bottom, cap slides L↔R on beat.
  int slotX = mx + 6, slotW = mw - 12, slotY = my + mh - 4;
  _t->drawFastHLine(slotX, slotY, slotW, CHASSIS_SH);
  int capX = slotX + (slide * (slotW - 4)) / 255;
  _t->fillRect(capX, slotY - 2, 4, 5, 0x05FF);
}

// ♪/♫ note particles rising on the left + right of the booth. Glyph and
// color cycle so it reads as a lively club. `t` advances the rise; `beat`
// gives an upward kick on each downbeat.
static void drawNotes(uint32_t t, uint8_t beat) {
  _t->setTextSize(1);
  for (int i = 0; i < 5; i++) {
    int phase = ((int)(t / 40) * 2 + i * 9) % 60;
    int baseX = (i & 1) ? (110 - i * 4) : (18 + i * 5);
    int y = 118 - phase - (beat >> 5);
    if (y < 2) continue;
    int x = baseX + (((phase / 4) & 1) ? 2 : -2);
    _t->setTextColor(DJ_COLORS[(i + (t / 200)) % 6], BUDDY_BG);
    _t->setCursor(x, y);
    // Single/double note glyphs from the default font's printable set.
    _t->print((i & 1) ? "\x0e" : "\x0d");   // ♫ / ♪ in the 8x8 ROM font
  }
}

// gr0mRenderDJ scene body — composed onto whatever surface _t points at.
// Called from the global trampoline below (which sets _t = tgt first).
static void djScene(uint32_t t) {
  readTilt();   // keep the head parallax alive even in DJ mode

  uint32_t phaseMs = t % DJ_BEAT_MS;
  uint8_t  beat    = djBeatEnvelope(phaseMs);
  uint32_t beatNum = t / DJ_BEAT_MS;

  // Head bob: dip down hard on the kick, spring back between beats.
  _yProjOff = -(beat >> 6);   // 0..-3 px

  // ── Decks + mixer (drawn first, behind the head/hands) ──────────────
  // Left deck spins steadily; right deck "scratches" — its angle jitters
  // back and forth on the beat instead of advancing smoothly.
  float spin    = (float)t * 0.012f;
  float scratch = spin + sinf((float)t * 0.05f) * (beat / 255.0f) * 2.2f;
  drawTurntable(26, 132, spin);
  drawTurntable(109, 132, scratch);
  // Crossfader rides L→R→L across two beats so it visibly travels.
  uint8_t slide = (beatNum & 1) ? (255 - beat) : beat;
  drawMixer(HX, 138, slide);

  // ── Hands on the decks — reuse the laptop-hands stubs, splayed wider
  // so they read as resting on the platters; right hand tracks scratch.
  {
    int ly = 120, ry = 120 + (beat >> 6);
    _t->drawLine(HX - 16, HY + 30, 30, ly, CHASSIS_SH);
    _t->drawLine(HX + 16, HY + 30, 105, ry, CHASSIS_SH);
    _t->fillCircle(30, ly, 3, CHASSIS);  _t->drawCircle(30, ly, 3, CHASSIS_SH);
    _t->fillCircle(105, ry, 3, CHASSIS); _t->drawCircle(105, ry, 3, CHASSIS_SH);
  }

  // ── gr0m head, bobbing, with headphones on ──────────────────────────
  drawHead3D();
  // Visor as a VU meter: 6 vertical bars with per-bar beat-driven heights
  // and cycling club colors (replaces the normal pupil). Drawn on the
  // front face so it rides the head rotation.
  if (frontFaceVisible()) {
    V2 vtl = onFace(-18, -7);
    V2 vbr = onFace( 18,  6);
    int vx = vtl.x, vy = vtl.y;
    int vw = vbr.x - vtl.x, vh = vbr.y - vtl.y;
    if (vw < 6) vw = 6;
    // Recessed frame
    _t->fillRect(vx - 1, vy - 1, vw + 2, vh + 2, INK);
    _t->fillRect(vx, vy, vw, vh, 0x0841);
    const int N = 6;
    int bw = vw / N;
    for (int i = 0; i < N; i++) {
      // Each bar's height is a phase-shifted slice of the beat envelope.
      uint8_t bv = djBeatEnvelope((phaseMs + i * (DJ_BEAT_MS / N)) % DJ_BEAT_MS);
      int bh = 2 + (bv * (vh - 2)) / 255;
      int bx = vx + i * bw + 1;
      _t->fillRect(bx, vy + vh - bh, bw - 1, bh, DJ_COLORS[(i + beatNum) % 6]);
    }
  }
  drawHeadphones();   // full-scale only (early-returns at scale 1)
  // Antenna LED pulses on the beat.
  drawAntenna3D(beat > 80 ? DJ_COLORS[beatNum % 6] : 0);

  // ── Note particles + BPM readout ────────────────────────────────────
  drawNotes(t, beat);
  // "124" BPM chest readout — small green LCD-style tag.
  _t->setTextSize(1);
  _t->fillRect(HX - 12, 160, 24, 11, INK);
  _t->fillRect(HX - 11, 161, 22, 9, 0x02E0);
  _t->setTextColor(0x5FE0, 0x02E0);
  _t->setCursor(HX - 8, 162);
  _t->print("124");
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
  if (evoStage() >= 4) {            // Stage 4 (HUD): sleep gear unlocks
    drawNightcap();                 // drooping sleep cap (HUD gear)
    drawZParticles(t);              // Zzz drifting up
  }
  drawMoodParticles(t, 2, 4);
}

static void doIdle(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawRide();   // #tilt-skate/surf/hover board (drawn first, under the bot)
  drawShadow(0);
  // Pixel desk + coffee mug under the bot — only paints in landscape clock
  // mode (drawDesk() self-gates on the render surface), matching the canvas
  // "AT HIS DESK" scene that pairs the desk with the sideways clock. Drawn
  // before the body so the bot sits in front of it.
  drawDesk();
  drawChest3D();
  // NEW: cycle the chest bolt with a tiny green LCD showing live token count.
  // 14 frames bolt, 6 frames LCD — keeps the brand mark dominant but lets
  // the readout flicker into view occasionally so idle reads as "alive,
  // counting".
  // The LCD readout is a Stage-4 (HUD) feature — drawChestLCD() itself
  // returns early below Stage 4. Only swap the bolt out for it once the
  // stage actually renders the LCD, otherwise stages 2-3 would lose the
  // brand bolt 1 frame in 4 (drawChestLCD draws nothing, leaving the
  // chest blank on lcdFrame frames).
  bool lcdFrame = evoStage() >= 4 && ((t / 5) % 4) == 0;
  if (lcdFrame) {
    // chest LCD swap — tokens with K/M shortening so it always fits 4 chars.
    // Source is stats().tokens (NVS-backed cumulative), not tama.tokens —
    // TamaState carries the live BLE feed, not the persistent counter.
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
  if (evoStage() < 5) drawSunglasses3D();   // disguise glasses replace these at Stage 5
  drawMouth3D(((t / 25) % 5 == 0) ? 2 : 0);
  drawAntenna3D(0);
  if (evoStage() >= 5) drawHumanDisguise(t);  // Stage 5 (Ascended): final form
  drawMoodParticles(t, 2, 3);
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
  drawVisor3D(VISOR_BUSY);
  drawMouth3D(3);                   // grimace
  drawAntenna3D(ledPulse ? VISOR_BUSY : 0);
  // Stage 4 (HUD): the work rig unlocks — headphones on, laptop + typing
  // hands in front. Below HUD the busy mood still renders, just bare.
  if (evoStage() >= 4) {
    drawLaptop(t);
    drawHandsAtLaptop();
    // Headphones occupy the head slot the fedora claims at Stage 5, so only
    // wear them at HUD (Stage 4); the disguise's hat wins the conflict above.
    if (evoStage() < 5) drawHeadphones();  // full-scale only (early-returns at scale 1)
  }
  // Stage 5 (Ascended): final form. The disguise headlines — the fedora
  // takes the head slot the headphones held (hat wins the conflict), but the
  // work rig (laptop + typing hands) still reads underneath the coat since it
  // doesn't fight the head gear.
  if (evoStage() >= 5) drawHumanDisguise(t);
  drawMoodParticles(t, 4, 1);
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
  drawVisor3D(VISOR_ALERT);
  if (evoStage() < 5) drawSunglasses3D();   // disguise glasses replace these at Stage 5
  drawMouth3D(4);                     // O shout
  drawAntenna3D(pulse ? VISOR_ALERT : 0);
  // Stage 5 (Ascended): final form. Wear the coat/fedora/glasses but keep
  // the alarm "!" bubble (below) as the attention tell so we don't stack two
  // bubbles — the disguise's own "HELLO HUMAN" bubble would fight the alert.
  // drawHumanTell() (which carries that bubble) is intentionally skipped here.
  if (evoStage() >= 5) {
    drawTrenchcoat();
    drawFedora();
    drawHumanGlasses();
  }
  drawMoodParticles(t, 5, 1);
  // Stage 4 (HUD): the alert speech bubble unlocks — blinks on alternate ticks.
  // Below HUD the attention mood still renders (red visor + "!" mouth), bare.
  if (pulse && evoStage() >= 4) drawSpeechBubble("!", VISOR_ALERT);
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
  drawSunglasses3D();
  drawMouth3D(2);                     // smile
  drawAntenna3D(RAINBOW[t % 6]);
  if (evoStage() >= 4) {              // Stage 4 (HUD): party gear unlocks
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
  if (evoStage() < 5) drawSunglasses3D();   // disguise glasses replace these at Stage 5
  drawMouth3D(6);                     // X
  drawAntenna3D((t & 3) == 0 ? CRIMSON : 0);
  if (evoStage() >= 5) drawHumanDisguise(t);  // Stage 5 (Ascended): final form
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
  drawVisor3D(VISOR_LOVE);
  if (evoStage() < 5) drawSunglasses3D();   // disguise glasses replace these at Stage 5
  drawMouth3D(((t / 8) & 1) ? 2 : 7);
  drawAntenna3D(((t / 3) & 1) ? HEART_RED : 0);
  if (evoStage() >= 4) drawHeartCloud(t);     // Stage 4 (HUD): heart-cloud gear
  if (evoStage() >= 5) drawHumanDisguise(t);  // Stage 5 (Ascended): final form
  drawMoodParticles(t, 4, 2);
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

// Alt human-costume easter-egg entry point — global so the main loop can
// call it without touching the gr0m namespace internals. Points the
// namespace's active render surface at `tgt` (mirrors gr0mRenderDJ), then
// composes the scene. `t` is millis(). No trigger is wired here — the caller
// decides when to show it.
void gr0mRenderHumanCostume(TFT_eSPI* tgt, uint32_t t) {
  gr0m::_t = tgt;
  gr0m::humanCostumeScene(t);
}

// Ride-mode control for main.cpp's "ride" row (#tilt-skate/surf/hover).
void gr0mCycleRide()   { gr0m::_rideMode = (gr0m::_rideMode + 1) % 4; }
uint8_t gr0mRideMode() { return gr0m::_rideMode; }

extern const Species GR0M_SPECIES = {
  "gr0m",
  0xF800,
  { gr0m::doSleep, gr0m::doIdle, gr0m::doBusy, gr0m::doAttention,
    gr0m::doCelebrate, gr0m::doDizzy, gr0m::doHeart }
};
