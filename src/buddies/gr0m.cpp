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
// elements (visor "pupil", antenna sway, shadow position, smoke drift)
// to create a fake-3D / parallax-depth effect as the device is tilted.
//   _tiltX:  -4..+4   negative = left tilt
//   _tiltY:  -4..+4   negative = forward (away from user) tilt
//   _gravX:  -2..+2   gravity component along screen x — drives smoke
static int8_t _tiltX = 0;
static int8_t _tiltY = 0;
static int8_t _gravX = 0;

static int8_t _clamp(float v, int lo, int hi) {
  int iv = (int)(v + (v >= 0 ? 0.5f : -0.5f));
  if (iv < lo) iv = lo;
  if (iv > hi) iv = hi;
  return (int8_t)iv;
}

// Forward decl — buildRotation body lives in the 3D pipeline section.
static void buildRotation(float yaw, float pitch);

static void readTilt() {
  float ax = 0, ay = 0, az = 0;
  M5.Imu.getAccelData(&ax, &ay, &az);
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
  // Build the actual 3D rotation matrix used by the cube renderer.
  // Yaw from ax (head turns left/right when device tilts side-to-side);
  // pitch from ay (head looks up/down when device pitches forward/back).
  // ±0.7 rad ≈ ±40° at max tilt — strong but not disorienting.
  float yaw   = -axS * 0.7f;
  float pitch =  ayS * 0.7f;
  buildRotation(yaw, pitch);
}

// Rigid-body face-plane offset. Every head-attached element (chassis,
// visor, glasses, mouth, joint base) adds this so they rotate TOGETHER
// as a single 3D plane. The chest stays anchored — head appears to
// pivot on the neck.
static int faceOffX() { return _tiltX; }       // ±8 px lateral shift
static int faceOffY() { return _tiltY / 2; }   // ±3 px vertical (forward/back lean)

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
  return { (int)(HX + v.x * FOCAL / z),
           (int)(ANCHOR_Y + _yProjOff + v.y * FOCAL / z) };
}

// Rotate + project in one call — common pattern.
static V2 rp(V3 v) { return projectV(rotateV(v)); }

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
//     • lit joint sticking out the mouth area with ember + rising smoke
//
//   BUSY state — gr0m gets serious:
//     • sunglasses come OFF — the bare cyan/green visor is exposed
//     • joint comes OUT of the mouth
//     • both items shown tumbling discarded at the upper corners
//       (sunglasses upper-left, joint upper-right with smoke trail)
//     • grimace mouth, antenna LED blinks
//
//   Mood particles (left side of canvas): red hearts rising → morph
//   into green cannabis leaves at the half-life of their flight.
// ════════════════════════════════════════════════════════════════════

// Palette (RGB565)
static const uint16_t CHASSIS    = 0xC638;  // brushed-steel body
static const uint16_t CHASSIS_SH = 0x8410;  // chassis shadow / panel-line
static const uint16_t CHASSIS_HI = 0xEF7D;  // chassis highlight
static const uint16_t INK        = 0x0000;  // sunglasses lens fill
static const uint16_t STEEL      = 0x73AE;  // outlines + lens frame
static const uint16_t SPECULAR      = 0xFFFF;
static const uint16_t CRIMSON    = 0xF800;  // antenna LED, accents
static const uint16_t EMBER_HOT  = 0xFD20;
static const uint16_t EMBER_DIM  = 0xC000;
static const uint16_t LEAF_GREEN = 0x0660;
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
  _t->fillCircle(x - 2, y - 1, 2, c);
  _t->fillCircle(x + 2, y - 1, 2, c);
  _t->fillTriangle(x - 3, y, x + 3, y, x, y + 4, c);
}

static void drawLeaf(int x, int y, uint16_t c) {
  _t->fillTriangle(x, y - 3, x - 2, y + 1, x + 2, y + 1, c);
  _t->drawPixel(x, y + 2, c);
  _t->drawPixel(x, y + 3, c);
}

// ── Robot body parts ────────────────────────────────────────────────

// Soft drop shadow beneath the robot — offset by tilt so it tracks the
// "light source" direction, giving a fake-3D "lifted off screen" feel.
static void drawShadow(int yOff) {
  if (buddyScale() == 1) return;   // 2D helpers use home HX/HY — skip in peek
  int sx = HX + _tiltX * 2;
  int sy = HY + 60 + yOff;
  // 3-ring soft shadow, darkest at center
  _t->fillEllipse(sx, sy + 2, 26, 3, 0x10A2);
  _t->fillEllipse(sx, sy + 1, 22, 2, 0x20C3);
  _t->fillEllipse(sx, sy,     18, 2, 0x2965);
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
static void drawAntenna3D(uint16_t ledColor) {
  if (evoStage() < 2) return;            // Stage 2 (Powered): antenna up
  V3 base = { 0, -22, 0 };
  V3 tip  = { 0, -36, 0 };
  drawPole3D(base, tip, CHASSIS_SH);
  V2 p = rp(tip);
  if (ledColor) {
    _t->fillCircle(p.x, p.y, 3, ledColor);
    _t->drawCircle(p.x, p.y, 3, SPECULAR);
    _t->drawCircle(p.x, p.y, 4, (ledColor >> 2) & 0x39E7);  // halo
    _t->drawPixel(p.x - 1, p.y - 1, SPECULAR);
  } else {
    _t->drawCircle(p.x, p.y, 3, CHASSIS_SH);
  }
}

// 3D joint — pole sticking out the mouth area on the front face.
// Only renders when the front face is visible (joint is "in front of" the head).
static void drawJoint3D(uint32_t t, bool lit) {
  if (evoStage() < 3) return;            // Stage 3 (Persona): the joint
  if (!frontFaceVisible()) return;
  // Object-space: emerges from front face (z = +22) near mouth (y = 14).
  // Extends forward AND right, slight droop with gravity.
  float dropY = (float)_gravX * 0.8f;
  V3 base = { 8.0f,  14.0f + dropY * 0.3f, 22.0f };
  V3 tip  = { 28.0f, 14.0f + dropY,        32.0f };
  drawPole3D(base, tip, 0xF79E);
  V2 baseP = rp(base);
  V2 tipP  = rp(tip);
  // Filter band near base — darker stub
  _t->drawLine(baseP.x, baseP.y, baseP.x + (tipP.x - baseP.x) / 5,
               baseP.y + (tipP.y - baseP.y) / 5, STEEL);
  // Ember at tip
  if (lit) {
    uint16_t glow = ((t / 2) & 1) ? EMBER_HOT : EMBER_DIM;
    _t->fillCircle(tipP.x, tipP.y, 2, glow);
    _t->drawPixel(tipP.x + 1, tipP.y - 1, EMBER_HOT);
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
  // Bright "pupil" — sits at face center plus tilt parallax
  V2 pup = onFace((float)_tiltX * 1.5f, -2);
  _t->fillRect(pup.x - 1, pup.y - 2, 3, 4, SPECULAR);
}

// 3D sunglasses — project lens centers onto the rotated face, draw
// filled circles. The circles don't squash to ellipses (would require
// projected-disc rendering), but their POSITIONS track the rotation.
static void drawSunglasses3D() {
  if (evoStage() < 3) return;            // Stage 3 (Persona): shades on
  if (!frontFaceVisible()) return;
  V2 lL = onFace(-12, -1);
  V2 lR = onFace( 12, -1);
  V2 bL = onFace(-4,  -1);
  V2 bR = onFace( 4,  -1);
  // Lens fills
  _t->fillCircle(lL.x, lL.y, 8, INK);
  _t->fillCircle(lR.x, lR.y, 8, INK);
  // Frames
  _t->drawCircle(lL.x, lL.y, 8, STEEL);
  _t->drawCircle(lL.x, lL.y, 9, STEEL);
  _t->drawCircle(lR.x, lR.y, 8, STEEL);
  _t->drawCircle(lR.x, lR.y, 9, STEEL);
  // Nose bridge — projected line between lens edges
  _t->drawLine(bL.x, bL.y, bR.x, bR.y, STEEL);
  _t->drawLine(bL.x, bL.y - 1, bR.x, bR.y - 1, STEEL);
  // Specular highlights
  V2 hL = onFace(-15, -5);
  V2 hR = onFace( 10, -5);
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
    case 4:  // O shouting
      _t->fillCircle(mC.x, mC.y + 1, 4, INK);
      _t->drawCircle(mC.x, mC.y + 1, 4, CHASSIS_SH);
      break;
    case 6: {  // X
      V2 a = onFace(-4, 12); V2 b = onFace( 4, 16);
      V2 c = onFace(-4, 16); V2 d = onFace( 4, 12);
      _t->drawLine(a.x, a.y, b.x, b.y, CHASSIS_SH);
      _t->drawLine(c.x, c.y, d.x, d.y, CHASSIS_SH);
      break;
    }
    case 7:  // bashful — small smile + blush
      _t->drawLine(mL.x + 4, mL.y, mR.x - 4, mR.y, CHASSIS_SH);
      _t->fillCircle(onFace(-9, 16).x, onFace(-9, 16).y, 1, HEART_RED);
      _t->fillCircle(onFace( 9, 16).x, onFace( 9, 16).y, 1, HEART_RED);
      break;
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
      _t->drawLine(mX - 6, mY, mX, mY + 3, EMBER_DIM);
      _t->drawLine(mX, mY + 3, mX + 6, mY, EMBER_DIM);
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

// Joint emerging from the mouth (default state). The joint TIP angles
// with gravity — tilt left and the lit end droops down-left, tilt right
// and it droops down-right. The shaft is drawn as a thin diagonal line
// instead of a horizontal rect, so the angle is visible.
static void drawJointInMouth(uint32_t t, bool lit, int yOff) {
  if (buddyScale() == 1) return;
  int fx = faceOffX();
  int fy = faceOffY();
  int jY = HY + 14 + yOff + fy;
  int jX0 = HX + 10 + fx;     // joint base attached to mouth corner
  // Tip droops with gravity — _gravX maps -3..+3 to ±6 px of vertical drop
  int dropY = _gravX * 2;
  int jX1 = HX + 30 + fx;     // tip extends from base, also shifted by face
  int tipY = jY + dropY;
  // Shaft — three parallel diagonal lines for thickness (tapered look)
  _t->drawLine(jX0,     jY - 1, jX1,     tipY - 1, 0xF79E);
  _t->drawLine(jX0,     jY,     jX1,     tipY,     0xF79E);
  _t->drawLine(jX0,     jY + 1, jX1,     tipY + 1, 0xF79E);
  // Steel outline along the top edge
  _t->drawLine(jX0,     jY - 1, jX1,     tipY - 1, STEEL);
  // Filter band near the mouth (darker, 4 px wide)
  _t->fillRect(jX0,     jY - 1, 4, 3, 0xA514);
  if (lit) {
    uint16_t glow = ((t / 2) & 1) ? EMBER_HOT : EMBER_DIM;
    _t->fillCircle(jX1 + 2, tipY, 2, glow);
    _t->drawPixel(jX1 + 4, tipY - 1, EMBER_HOT);
  }
}

// Smoke rising from the joint tip. Smoke rises OPPOSITE to gravity,
// so it drifts in the direction _away_ from the gravity vector.
// Also follows the joint tip's drop angle so the smoke trail starts
// at the actual ember position.
static void drawSmokeFromMouth(uint32_t t, int intensity, int yOff) {
  if (evoStage() < 3) return;            // Stage 3 (Persona): smoke
  if (buddyScale() == 1) return;
  if (intensity <= 0) return;
  int fx = faceOffX();
  int fy = faceOffY();
  int dropY = _gravX * 2;
  int sX = HX + 32 + fx;
  int sY = HY + 12 + yOff + fy + dropY;     // follow ember
  // Smoke drift opposite gravity (negative for tilt-right, positive
  // for tilt-left — so it appears to "rise straight up" in world frame
  // even when the device is tilted)
  int driftX = -_gravX * 2;
  int n = (intensity == 1) ? 3 : (intensity == 2) ? 5 : 7;
  for (int i = 0; i < n; i++) {
    int phase = ((int)(t * 2) + i * 7) % 36;
    int y = sY - 2 - phase;
    if (y < 0) continue;
    int curl = ((phase / 4) & 1) ? 1 : -1;
    // Drift accumulates with height — higher particles drifted further
    int x = sX + curl + (phase / 5) + (driftX * phase) / 12;
    int r = (phase < 8) ? 2 : 1;
    uint16_t color = (phase < 6) ? 0xF79E
                   : (phase < 16) ? STEEL
                                  : 0x39E7;
    _t->fillCircle(x, y, r, color);
  }
}

// ── BUSY-only: discarded items tumbling at the upper corners ───────

// Glasses tumbling in upper-left, cycles through 4 orientation poses
static void drawDiscardedGlasses(uint32_t t) {
  if (buddyScale() == 1) return;
  // Static base position with small arc bob
  static const int8_t BOB_X[4] = { 0, 2, 4, 2 };
  static const int8_t BOB_Y[4] = { 0, -2, 0, 2 };
  uint8_t pose = (t / 3) % 4;
  int x = 16 + BOB_X[pose];
  int y = 18 + BOB_Y[pose];
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

// Joint tumbling in upper-right with trailing smoke
static void drawDiscardedJoint(uint32_t t) {
  if (buddyScale() == 1) return;
  static const int8_t BOB_X[4] = { 0, -2, -4, -2 };
  static const int8_t BOB_Y[4] = { 0, -3, 0, 3 };
  uint8_t pose = (t / 3) % 4;
  int x = 115 + BOB_X[pose];
  int y = 16 + BOB_Y[pose];
  // Joint at 4 orientations
  switch (pose) {
    case 0:  // horizontal, ember-right
      _t->fillRect(x - 6, y - 1, 12, 3, 0xF79E);
      _t->drawRect(x - 6, y - 1, 12, 3, STEEL);
      _t->fillCircle(x + 7, y, 2, EMBER_HOT);
      break;
    case 1:  // diagonal up-right
      _t->drawLine(x - 5, y + 4, x + 5, y - 4, 0xF79E);
      _t->drawLine(x - 5, y + 5, x + 5, y - 3, 0xF79E);
      _t->drawLine(x - 5, y + 3, x + 5, y - 5, STEEL);
      _t->fillCircle(x + 6, y - 5, 2, EMBER_HOT);
      break;
    case 2:  // vertical
      _t->fillRect(x - 1, y - 6, 3, 12, 0xF79E);
      _t->drawRect(x - 1, y - 6, 3, 12, STEEL);
      _t->fillCircle(x, y - 7, 2, EMBER_HOT);
      break;
    case 3:  // diagonal down-right
      _t->drawLine(x - 5, y - 4, x + 5, y + 4, 0xF79E);
      _t->drawLine(x - 5, y - 3, x + 5, y + 5, 0xF79E);
      _t->drawLine(x - 5, y - 5, x + 5, y + 3, STEEL);
      _t->fillCircle(x + 6, y + 5, 2, EMBER_HOT);
      break;
  }
  // Smoke trail from the discarded joint
  for (int i = 0; i < 4; i++) {
    int p = (t + i * 5) % 24;
    int sy = y - 4 - p;
    if (sy < 0) continue;
    int sx = x + 2 + ((p / 3) & 1 ? 1 : -1);
    uint16_t c = (p < 8) ? 0xF79E : (p < 16) ? STEEL : 0x39E7;
    _t->fillCircle(sx, sy, p < 8 ? 2 : 1, c);
  }
  // Motion lines suggesting toss
  uint16_t blur = 0x4208;
  _t->drawPixel(x + 10, y, blur);
  _t->drawPixel(x + 12, y - 1, blur);
  _t->drawPixel(x + 14, y, blur);
}

// ── Mood particles ──────────────────────────────────────────────────
// Hearts → leaves. Hearts rise on the left, morphing to green leaves
// at the half-life of their flight.
static void drawMoodParticles(uint32_t t, int n, int speed) {
  if (evoStage() < 4) return;            // Stage 4 (HUD): mood particles
  if (buddyScale() == 1) return;
  const int LIFECYCLE = 28;
  const int MORPH = LIFECYCLE / 2;
  int baseX = -32;
  for (int i = 0; i < n; i++) {
    int phase = ((int)t * 2 / speed + i * 5) % LIFECYCLE;
    int y = HY + 38 - phase;
    if (y < 2 || y > HY + 40) continue;
    int x = HX + baseX + (n > 1 ? (i * 22) / (n - 1) : 0);
    x += ((phase / 2) & 1) ? 1 : -1;
    // Drift with gravity — particles ride the same world-frame "up"
    // axis as the smoke, so they all feel like a coherent atmosphere.
    x += (-_gravX * phase) / 8;
    if (phase < MORPH) {
      drawHeart(x, y, HEART_RED);
    } else {
      drawLeaf(x, y, LEAF_GREEN);
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
  // Band sweeps from upper-left to upper-right of the head
  int topY = HY - HH/2 + fy - 6;
  for (int i = -22; i <= 22; i++) {
    // parabolic arc — y = topY - (1 - (i/22)^2) * 4
    float n = (float)i / 22.0f;
    int yOff = (int)((1.0f - n * n) * 4.0f);
    _t->drawPixel(cx + i, topY - yOff, STEEL);
    _t->drawPixel(cx + i, topY - yOff + 1, STEEL);
  }
  // Ear cups — fixed (don't rotate with face) so they read like real cans
  int eY = HY - 4 + fy;
  _t->fillRect(cx - 28, eY,     6, 12, STEEL);
  _t->fillRect(cx + 22, eY,     6, 12, STEEL);
  // Red foam ring stripe (visible because slightly inset)
  _t->fillRect(cx - 23, eY + 4, 1, 4, CRIMSON);
  _t->fillRect(cx + 22, eY + 4, 1, 4, CRIMSON);
  // Subtle highlight on the cups
  _t->drawPixel(cx - 28, eY,     SPECULAR);
  _t->drawPixel(cx + 27, eY,     SPECULAR);
}

// Small hands at the chest area — used when typing.
// Two stubby grey circles either side of the laptop, with thin arms back
// to the chest body.
static void drawHandsAtLaptop() {
  int cx = HX;
  int hy = HY + 38;
  // Arms (thin chassis-color stubs)
  _t->drawLine(cx - 20, HY + 30, cx - 14, hy - 2, CHASSIS_SH);
  _t->drawLine(cx - 19, HY + 30, cx - 13, hy - 2, CHASSIS_SH);
  _t->drawLine(cx + 20, HY + 30, cx + 14, hy - 2, CHASSIS_SH);
  _t->drawLine(cx + 19, HY + 30, cx + 13, hy - 2, CHASSIS_SH);
  // Hands (circles)
  _t->fillCircle(cx - 14, hy, 3, CHASSIS);
  _t->drawCircle(cx - 14, hy, 3, CHASSIS_SH);
  _t->drawPixel(cx - 15, hy - 1, CHASSIS_HI);
  _t->fillCircle(cx + 14, hy, 3, CHASSIS);
  _t->drawCircle(cx + 14, hy, 3, CHASSIS_SH);
  _t->drawPixel(cx + 13, hy - 1, CHASSIS_HI);
}

// Tiny laptop sat in front of gr0m, glowing green screen with fake code.
// Drawn between chest and bottom edge of screen so it reads as "on lap".
static void drawLaptop(uint32_t t) {
  if (buddyScale() == 1) return;
  int cx = HX;
  int kx = cx - 20, ky = HY + 36;  // keyboard base
  int kw = 40, kh = 3;
  // Keyboard base + lip
  _t->fillRect(kx, ky, kw, kh, 0x3186);
  _t->drawFastHLine(kx, ky + kh, kw, INK);
  // Key hint dots
  for (int i = 0; i < 8; i++) _t->drawPixel(kx + 3 + i * 5, ky + 1, 0x7BEF);
  // Hinge
  _t->drawFastHLine(kx + 2, ky - 1, kw - 4, 0x52AA);
  // Screen bezel
  int sx = kx + 4, sy = ky - 14;
  int sw = kw - 8, sh = 13;
  _t->fillRect(sx, sy, sw, sh, INK);
  _t->fillRect(sx + 1, sy + 1, sw - 2, sh - 2, 0x0660);
  // Fake code lines — light green pixels in a vague pattern
  _t->drawFastHLine(sx + 2, sy + 2, 6, 0x07E0);
  _t->drawFastHLine(sx + 9, sy + 2, 4, 0x07E0);
  _t->drawFastHLine(sx + 2, sy + 4, 3, 0x07E0);
  _t->drawFastHLine(sx + 6, sy + 4, 8, 0x07E0);
  _t->drawFastHLine(sx + 2, sy + 6, 10, 0x07E0);
  _t->drawFastHLine(sx + 13, sy + 6, 2, 0x07E0);
  _t->drawFastHLine(sx + 2, sy + 8, 5, 0x07E0);
  // Blinking cursor
  if ((t / 2) & 1) _t->fillRect(sx + 14, sy + 8, 2, 1, SPECULAR);
}

// Speech bubble pointing at gr0m's head. White rounded rect with black
// outline and a tail. Text rendered in `textColor`. Position auto-clamps
// to keep the bubble inside the 135px-wide screen, even with long text.
static void drawSpeechBubble(const char* text, uint16_t textColor) {
  if (buddyScale() == 1) return;
  int len = 0; while (text[len]) len++;
  int w = len * 6 + 6;
  // Place to the upper-right of the head, but clamp so right edge ≤ 134.
  int x = HX + 30;
  if (x + w > 134) x = 134 - w;
  if (x < 1) x = 1;
  int y = HY - 28;
  if (y < 1) y = 1;
  // body
  _t->fillRoundRect(x, y, w, 12, 2, SPECULAR);
  _t->drawRoundRect(x, y, w, 12, 2, INK);
  // tail (always points left-down toward the head)
  _t->fillTriangle(x + 3, y + 11, x + 9, y + 11, x, y + 16, SPECULAR);
  _t->drawLine(x + 3, y + 11, x, y + 16, INK);
  _t->drawLine(x + 9, y + 11, x, y + 16, INK);
  // text
  _t->setTextSize(1);
  _t->setTextColor(textColor, SPECULAR);
  _t->setCursor(x + 3, y + 3);
  _t->print(text);
}

// Party hat — pointy triangle on top of head with stripes and a pom.
static void drawPartyHat() {
  if (buddyScale() == 1) return;
  int fx = faceOffX();
  int fy = faceOffY();
  int hx = HX + fx;
  int hy = HY - HH/2 + fy;
  _t->fillTriangle(hx, hy - 18, hx - 10, hy - 2, hx + 10, hy - 2, CRIMSON);
  _t->drawTriangle(hx, hy - 18, hx - 10, hy - 2, hx + 10, hy - 2, INK);
  // Stripes
  _t->drawLine(hx - 5, hy - 9, hx + 5, hy - 9, SPECULAR);
  _t->drawLine(hx - 7, hy - 5, hx + 7, hy - 5, SPECULAR);
  // Pom pom
  _t->fillCircle(hx, hy - 19, 2, 0xFFE0);
  _t->drawPixel(hx, hy - 20, SPECULAR);
}

// Nightcap — drooping cap that hangs to the right, white trim band, pom.
static void drawNightcap() {
  if (buddyScale() == 1) return;
  int fx = faceOffX();
  int fy = faceOffY();
  int hx = HX + fx;
  int hy = HY - HH/2 + fy;
  // White trim band wrapping the head top
  _t->fillRect(hx - 24, hy - 1, 48, 4, SPECULAR);
  _t->drawFastHLine(hx - 24, hy - 1, 48, 0xC638);
  // Drooping body — curves up and to the right
  for (int i = 0; i < 18; i++) {
    int y = hy - 3 - i;
    int xLeft  = hx - 18 + i * 2;
    int xRight = hx - 8 + i * 2;
    _t->drawFastHLine(xLeft, y, xRight - xLeft, VISOR_LOVE);
  }
  // Pom pom at the tip
  int px = hx + 18, py = hy - 22;
  _t->fillCircle(px, py, 3, SPECULAR);
  _t->drawPixel(px - 1, py - 1, 0xC638);
}

// Z particles drifting up — used in sleep state.
static void drawZParticles(uint32_t t) {
  if (buddyScale() == 1) return;
  _t->setTextColor(SPECULAR, BUDDY_BG);
  _t->setTextSize(1);
  int p1 = (int)(t * 2) % 30;
  if (p1 < 26) {
    _t->setCursor(HX + 12, HY - 16 - p1);
    _t->print("z");
  }
  int p2 = (int)((t + 5) * 2) % 30;
  if (p2 < 26) {
    _t->setTextSize(2);
    _t->setCursor(HX + 18, HY - 24 - p2);
    _t->print("Z");
    _t->setTextSize(1);
  }
}

// Confetti rain — for celebrate.
static void drawConfetti(uint32_t t) {
  if (buddyScale() == 1) return;
  static const uint16_t CONF_COL[] = { HEART_RED, 0xFFE0, VISOR_IDLE, VISOR_BUSY, EMBER_HOT };
  for (int i = 0; i < 14; i++) {
    int phase = ((int)t * 2 + i * 11) % 40;
    int x = (i * 11 + (int)t * 3) % 135;
    int y = phase * 4;
    if (y < 0 || y > 132) continue;
    uint16_t c = CONF_COL[i % 5];
    if (i & 1) _t->fillRect(x, y, 2, 3, c);
    else       _t->fillCircle(x, y, 1, c);
  }
}

// Extra heart cloud — sparser but with bigger hearts. Heart state only.
static void drawHeartCloud(uint32_t t) {
  if (buddyScale() == 1) return;
  static const int8_t HPOS[][2] = {
    {18, 50}, {28, 28}, {10, 92}, {110, 38}, {118, 72}, {102, 100}
  };
  for (int i = 0; i < 6; i++) {
    int phase = ((int)t * 2 + i * 7) % 24;
    int y = HPOS[i][1] - phase;
    int x = HPOS[i][0] + ((i & 1) ? 1 : -1);
    if (y < 0) continue;
    drawHeart(x, y, HEART_RED);
  }
}

// Optional chest LCD — replaces the bolt with a small green readout.
// Opt-in via settings (e.g. settings.chestLcd = true). text is ≤4 chars
// to fit in the 20×11 inset.
static void drawChestLCD(const char* text) {
  if (evoStage() < 4) return;            // Stage 4 (HUD): chest readout
  // Wide enough for a 4-char K/M figure ("729K", "12M") without clipping.
  int lx = HX - 15, ly = HY + 30;
  int lw = 30, lh = 11;
  _t->fillRect(lx - 1, ly - 1, lw + 2, lh + 2, INK);
  _t->fillRect(lx, ly, lw, lh, 0x02E0);
  // 1px lit border
  _t->drawRect(lx, ly, lw, lh, 0x0660);
  _t->setTextSize(1);
  _t->setTextColor(0x5FE0, 0x02E0);
  int textLen = 0; while (text[textLen]) textLen++;
  int textW = textLen * 6;
  _t->setCursor(lx + (lw - textW) / 2, ly + 2);
  _t->print(text);
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
  V2 cL0 = onChest(-12, 26);
  V2 cL1 = onChest(-3, 18);
  V2 cL2 = onChest(-8, 26);
  V2 cR0 = onChest( 12, 26);
  V2 cR1 = onChest( 3, 18);
  V2 cR2 = onChest( 8, 26);
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
  V2 bL = onFace(-26, -18);
  V2 bR = onFace( 26, -18);
  V2 bF = onFace(  0, -14);    // front lip dips toward the viewer
  V2 bB = onFace(  0, -22);    // back edge
  _t->fillTriangle(bL.x, bL.y, bR.x, bR.y, bF.x, bF.y, HAT_BROWN);
  _t->fillTriangle(bL.x, bL.y, bR.x, bR.y, bB.x, bB.y, HAT_BROWN);
  _t->drawLine(bL.x, bL.y, bF.x, bF.y, HAT_SH);
  _t->drawLine(bR.x, bR.y, bF.x, bF.y, HAT_SH);
  // Crown — trapezoid sitting on the brim, with a pinched dent on top.
  V2 kBL = onFace(-15, -19);
  V2 kBR = onFace( 15, -19);
  V2 kTL = onFace(-11, -31);
  V2 kTR = onFace( 11, -31);
  _t->fillTriangle(kBL.x, kBL.y, kBR.x, kBR.y, kTR.x, kTR.y, HAT_BROWN);
  _t->fillTriangle(kBL.x, kBL.y, kTR.x, kTR.y, kTL.x, kTL.y, HAT_BROWN);
  // Pinch dent — a darker crease down the crown center.
  V2 dT = onFace(0, -31);
  V2 dB = onFace(0, -23);
  _t->drawLine(dT.x, dT.y, dB.x, dB.y, HAT_SH);
  // Hatband — dark stripe around the base of the crown.
  V2 hbL = onFace(-15, -20);
  V2 hbR = onFace( 15, -20);
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
  // Two rounded-rectangle lenses sitting over the visor band (y≈-1).
  V2 lTL = onFace(-19, -6);  V2 lBR = onFace(-3, 4);
  V2 rTL = onFace(  3, -6);  V2 rBR = onFace(19, 4);
  // Left lens
  _t->fillRect(lTL.x, lTL.y, lBR.x - lTL.x, lBR.y - lTL.y, INK);
  _t->drawRect(lTL.x - 1, lTL.y - 1, (lBR.x - lTL.x) + 2, (lBR.y - lTL.y) + 2, STEEL);
  _t->drawRect(lTL.x, lTL.y, lBR.x - lTL.x, lBR.y - lTL.y, STEEL);
  // Right lens
  _t->fillRect(rTL.x, rTL.y, rBR.x - rTL.x, rBR.y - rTL.y, INK);
  _t->drawRect(rTL.x - 1, rTL.y - 1, (rBR.x - rTL.x) + 2, (rBR.y - rTL.y) + 2, STEEL);
  _t->drawRect(rTL.x, rTL.y, rBR.x - rTL.x, rBR.y - rTL.y, STEEL);
  // Heavy nose bridge between the lenses.
  V2 brL = onFace(-3, -2);
  V2 brR = onFace( 3, -2);
  _t->drawLine(brL.x, brL.y, brR.x, brR.y, STEEL);
  _t->drawLine(brL.x, brL.y + 1, brR.x, brR.y + 1, STEEL);
  // Temple arms running back toward the ears (track parallax slightly).
  V2 tL = onFace(-25, -3);
  V2 tR = onFace( 25, -3);
  _t->drawLine(lTL.x - 1, brL.y, tL.x, tL.y, STEEL);
  _t->drawLine(rBR.x + 1, brR.y, tR.x, tR.y, STEEL);
  // Single specular glint so the lenses read as glass, parallax-shifted.
  V2 gl = onFace(-16 + (float)_tiltX, -4);
  _t->drawPixel(gl.x, gl.y, SPECULAR);
  _t->drawPixel(gl.x + 1, gl.y, SPECULAR);
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
  // Stiff fake smile under the glasses — a too-wide flat grin on the front
  // face. Only when the face is toward us, like the other face decorations.
  if (!frontFaceVisible()) return;
  V2 sL = onFace(-9, 15);
  V2 sR = onFace( 9, 15);
  V2 cL = onFace(-9, 12);    // corners hitched up — forced smile
  V2 cR = onFace( 9, 12);
  _t->drawLine(sL.x, sL.y, sR.x, sR.y, CHASSIS_SH);
  _t->drawLine(sL.x, sL.y, cL.x, cL.y, CHASSIS_SH);
  _t->drawLine(sR.x, sR.y, cR.x, cR.y, CHASSIS_SH);
}

// One-call composite for the Stage-5 disguise everyday look. Order matters:
// coat first (body layer), then the head gear over the chassis, then the
// tell on top of everything.
static void drawHumanDisguise(uint32_t t) {
  drawTrenchcoat();
  drawFedora();
  drawHumanGlasses();
  drawHumanTell(t);
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
  if (evoStage() >= 5) {            // Stage 5 (Ascended): sleep costume
    drawNightcap();                 // drooping sleep cap
    drawZParticles(t);              // Zzz drifting up
  }
  drawMoodParticles(t, 2, 4);
}

static void doIdle(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawShadow(0);
  drawChest3D();
  // NEW: cycle the chest bolt with a tiny green LCD showing live token count.
  // 14 frames bolt, 6 frames LCD — keeps the brand mark dominant but lets
  // the readout flicker into view occasionally so idle reads as "alive,
  // counting".
  bool lcdFrame = ((t / 5) % 4) == 0;
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
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_IDLE);
  if (evoStage() < 5) drawSunglasses3D();   // disguise glasses replace these at Stage 5
  drawMouth3D(((t / 25) % 5 == 0) ? 2 : 0);
  drawJoint3D(t, true);
  drawAntenna3D(0);
  drawSmokeFromMouth(t, 2, 0);
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
  // Stage 5 (Ascended): final form. The disguise headlines — the fedora
  // takes the head slot that headphones used to hold (hat wins the conflict),
  // but the work rig (laptop + typing hands) still reads underneath the coat
  // since it doesn't fight the head gear.
  if (evoStage() >= 5) {
    drawLaptop(t);
    drawHandsAtLaptop();
    drawHumanDisguise(t);
  }
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
  drawJoint3D(t, true);
  drawAntenna3D(pulse ? VISOR_ALERT : 0);
  drawSmokeFromMouth(t, 1, 0);
  // Stage 5 (Ascended): final form. Wear the coat/fedora/glasses but keep
  // the alarm "!" as the attention tell so we don't stack two bubbles — the
  // disguise's own "HELLO HUMAN" bubble would fight the alert here.
  if (evoStage() >= 5) {
    drawTrenchcoat();
    drawFedora();
    drawHumanGlasses();
  }
  drawMoodParticles(t, 5, 1);
  // NEW: pixel speech bubble — blinks on every other tick.
  if (pulse && evoStage() >= 5) drawSpeechBubble("!", VISOR_ALERT);  // Stage 5
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
  drawJoint3D(t, true);
  drawAntenna3D(RAINBOW[t % 6]);
  drawSmokeFromMouth(t, 2, _yProjOff);
  if (evoStage() >= 5) {              // Stage 5 (Ascended): party costume
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
  drawJoint3D(t, false);              // extinguished
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
  drawJoint3D(t, true);
  drawAntenna3D(((t / 3) & 1) ? HEART_RED : 0);
  drawSmokeFromMouth(t, 2, 0);
  if (evoStage() >= 5) {            // Stage 5 (Ascended): final form + heart cloud
    drawHeartCloud(t);
    drawHumanDisguise(t);
  }
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

extern const Species GR0M_SPECIES = {
  "gr0m",
  0xF800,
  { gr0m::doSleep, gr0m::doIdle, gr0m::doBusy, gr0m::doAttention,
    gr0m::doCelebrate, gr0m::doDizzy, gr0m::doHeart }
};
