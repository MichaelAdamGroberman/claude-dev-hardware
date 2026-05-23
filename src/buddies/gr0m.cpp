#include "../buddy.h"
#include "../buddy_common.h"
#include <M5StickCPlus.h>

extern TFT_eSprite spr;

namespace gr0m {

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
  _tiltX = _clamp(axS *  8.0f, -8, 8);
  _tiltY = _clamp(ayS *  6.0f, -6, 6);
  _gravX = _clamp(axS *  3.0f, -3, 3);
  // Build the actual 3D rotation matrix used by the cube renderer.
  // Yaw from ax (head turns left/right when device tilts side-to-side);
  // pitch from ay (head looks up/down when device pitches forward/back).
  // ±0.7 rad ≈ ±40° at max tilt — strong but not disorienting.
  float yaw   = -axS * 0.7f;
  float pitch =  ayS * 0.5f;
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
static V2 projectV(V3 v) {
  const float CAM_DIST = 90.0f;
  const float FOCAL    = 75.0f;
  float z = v.z + CAM_DIST;
  if (z < 1.0f) z = 1.0f;
  return { (int)(HX + v.x * FOCAL / z),
           (int)(HY + _yProjOff + v.y * FOCAL / z) };
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
  for (int f = 0; f < 6; f++) {
    uint8_t lit = faceLighting(faces[f].nx, faces[f].ny, faces[f].nz);
    drawQuad3D(v[faces[f].a], v[faces[f].b], v[faces[f].c], v[faces[f].d],
               shade(baseColor, lit), edge);
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
  drawCube3D({0, 36, 0}, 22.0f, 9.0f, 12.0f, CHASSIS, CHASSIS_SH);
}

// Small 3D neck cube between head and chest. Doesn't need lighting
// detail — small enough to read as a connector.
static void drawNeck3D() {
  drawCube3D({0, 23, 0}, 4.0f, 4.0f, 4.0f, CHASSIS_SH, CHASSIS_SH);
}

// 3D antenna — pole rising from top of head with a glowing LED ball.
static void drawAntenna3D(uint16_t ledColor) {
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
  drawSunglasses3D();
  drawMouth3D(1);                   // closed flat
  drawAntenna3D(0);                 // LED off
  drawMoodParticles(t, 2, 4);
}

static void doIdle(uint32_t t) {
  _t = buddyTarget();
  readTilt();
  _yProjOff = 0;
  drawShadow(0);
  drawChest3D();
  drawBolt3D(VISOR_IDLE);
  drawNeck3D();
  drawHead3D();
  drawVisor3D(VISOR_IDLE);
  drawSunglasses3D();
  drawMouth3D(((t / 25) % 5 == 0) ? 2 : 0);
  drawJoint3D(t, true);
  drawAntenna3D(0);
  drawSmokeFromMouth(t, 2, 0);
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
  drawMouth3D(3);                     // grimace
  drawAntenna3D(ledPulse ? VISOR_BUSY : 0);
  // (no shades, no joint — tossed)
  // 2D tumbling discarded items at upper corners
  drawDiscardedGlasses(t);
  drawDiscardedJoint(t);
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
  drawSunglasses3D();
  drawMouth3D(4);                     // O shout
  drawJoint3D(t, true);
  drawAntenna3D(pulse ? VISOR_ALERT : 0);
  drawSmokeFromMouth(t, 1, 0);
  drawMoodParticles(t, 5, 1);
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
  drawSunglasses3D();
  drawMouth3D(6);                     // X
  drawJoint3D(t, false);              // extinguished
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
  drawVisor3D(VISOR_LOVE);
  drawSunglasses3D();
  drawMouth3D(((t / 8) & 1) ? 2 : 7);
  drawJoint3D(t, true);
  drawAntenna3D(((t / 3) & 1) ? HEART_RED : 0);
  drawSmokeFromMouth(t, 2, 0);
  drawMoodParticles(t, 4, 2);
}

}  // namespace gr0m

extern const Species GR0M_SPECIES = {
  "gr0m",
  0xF800,
  { gr0m::doSleep, gr0m::doIdle, gr0m::doBusy, gr0m::doAttention,
    gr0m::doCelebrate, gr0m::doDizzy, gr0m::doHeart }
};
