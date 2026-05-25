// Host stub of M5StickCPlus.h / TFT_eSPI — implements only the draw
// primitives buddy.cpp + gr0m.cpp call, rasterizing into an RGB565
// framebuffer so the real character code can be rendered off-device.
// NOT used in the firmware build.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include "Arduino.h"

namespace _rgfx {
  static inline int imin(int a, int b) { return a < b ? a : b; }
  static inline int imax(int a, int b) { return a > b ? a : b; }
}

class TFT_eSPI {
public:
  uint16_t* _fb = nullptr;
  int _w = 0, _h = 0;
  int _cx = 0, _cy = 0;
  uint16_t _fg = 0xFFFF, _bg = 0x0000;

  void setFB(uint16_t* fb, int w, int h) { _fb = fb; _w = w; _h = h; }

  inline void px(int x, int y, uint16_t c) {
    if (!_fb || x < 0 || y < 0 || x >= _w || y >= _h) return;
    _fb[y * _w + x] = c;
  }

  void drawPixel(int x, int y, uint16_t c) { px(x, y, c); }
  void drawFastHLine(int x, int y, int w, uint16_t c) { for (int i = 0; i < w; i++) px(x + i, y, c); }
  void drawFastVLine(int x, int y, int h, uint16_t c) { for (int j = 0; j < h; j++) px(x, y + j, c); }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, c);
  }
  void drawRect(int x, int y, int w, int h, uint16_t c) {
    drawFastHLine(x, y, w, c); drawFastHLine(x, y + h - 1, w, c);
    drawFastVLine(x, y, h, c); drawFastVLine(x + w - 1, y, h, c);
  }
  void drawLine(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
      px(x0, y0, c);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  void fillCircle(int x0, int y0, int r, uint16_t c) {
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++)
      if (x * x + y * y <= r * r + r) px(x0 + x, y0 + y, c);
  }
  void drawCircle(int x0, int y0, int r, uint16_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
      px(x0 + x, y0 + y, c); px(x0 + y, y0 + x, c); px(x0 - y, y0 + x, c); px(x0 - x, y0 + y, c);
      px(x0 - x, y0 - y, c); px(x0 - y, y0 - x, c); px(x0 + y, y0 - x, c); px(x0 + x, y0 - y, c);
      y++; if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
  }
  void fillEllipse(int x0, int y0, int rx, int ry, uint16_t c) {
    if (rx <= 0 || ry <= 0) return;
    for (int y = -ry; y <= ry; y++) for (int x = -rx; x <= rx; x++) {
      double a = (double)x / rx, b = (double)y / ry;
      if (a * a + b * b <= 1.0) px(x0 + x, y0 + y, c);
    }
  }
  void fillCircleQuad(int x0, int y0, int r, int q, uint16_t c) {
    for (int y = 0; y <= r; y++) for (int x = 0; x <= r; x++) if (x * x + y * y <= r * r + r) {
      if (q & 1) px(x0 - x, y0 - y, c); if (q & 2) px(x0 + x, y0 - y, c);
      if (q & 4) px(x0 - x, y0 + y, c); if (q & 8) px(x0 + x, y0 + y, c);
    }
  }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    fillRect(x + r, y, w - 2 * r, h, c);
    fillRect(x, y + r, r, h - 2 * r, c); fillRect(x + w - r, y + r, r, h - 2 * r, c);
    fillCircleQuad(x + r, y + r, r, 1, c); fillCircleQuad(x + w - r - 1, y + r, r, 2, c);
    fillCircleQuad(x + r, y + h - r - 1, r, 4, c); fillCircleQuad(x + w - r - 1, y + h - r - 1, r, 8, c);
  }
  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    drawFastHLine(x + r, y, w - 2 * r, c); drawFastHLine(x + r, y + h - 1, w - 2 * r, c);
    drawFastVLine(x, y + r, h - 2 * r, c); drawFastVLine(x + w - 1, y + r, h - 2 * r, c);
    // simple filled-circle-quad outlines are visually close enough at this size
    for (int a = 0; a <= r; a++) {
      int b = (int)(sqrt((double)(r * r - a * a)) + 0.5);
      px(x + r - a, y + r - b, c); px(x + w - r - 1 + a, y + r - b, c);
      px(x + r - a, y + h - r - 1 + b, c); px(x + w - r - 1 + a, y + h - r - 1 + b, c);
    }
  }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) {
    using namespace _rgfx;
    int minx = imin(x0, imin(x1, x2)), maxx = imax(x0, imax(x1, x2));
    int miny = imin(y0, imin(y1, y2)), maxy = imax(y0, imax(y1, y2));
    auto edge = [](int ax, int ay, int bx, int by, int px_, int py_) {
      return (bx - ax) * (py_ - ay) - (by - ay) * (px_ - ax);
    };
    long area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) { drawLine(x0, y0, x1, y1, col); drawLine(x1, y1, x2, y2, col); return; }
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
      long w0 = edge(x1, y1, x2, y2, x, y), w1 = edge(x2, y2, x0, y0, x, y), w2 = edge(x0, y0, x1, y1, x, y);
      bool inside = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
      if (inside) px(x, y, col);
    }
  }
  void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    drawLine(x0, y0, x1, y1, c); drawLine(x1, y1, x2, y2, c); drawLine(x2, y2, x0, y0, c);
  }

  // text: geometry-only renderer, so text is a no-op
  void setTextColor(uint16_t c) { _fg = c; }
  void setTextColor(uint16_t c, uint16_t b) { _fg = c; _bg = b; }
  void setTextSize(uint8_t) {}
  void setTextFont(int) {}
  void setTextDatum(uint8_t) {}
  void setCursor(int x, int y) { _cx = x; _cy = y; }
  void print(const char*) {}
  void print(char) {}
  void printf(const char*, ...) {}
  void drawString(const char*, int, int) {}

  void fillSprite(uint16_t c) { if (_fb) for (int i = 0; i < _w * _h; i++) _fb[i] = c; }
  void fillScreen(uint16_t c) { fillSprite(c); }
  void pushSprite(int, int) {}
  void setSwapBytes(bool) {}
  void setRotation(uint8_t) {}
  int width() { return _w; }
  int height() { return _h; }
};

class TFT_eSprite : public TFT_eSPI {
public:
  TFT_eSprite(TFT_eSPI* = nullptr) {}
  void* createSprite(int, int) { return nullptr; }   // framebuffer set via setFB()
  void deleteSprite() {}
  void setColorDepth(int) {}
};

// ── M5 device stub ──────────────────────────────────────────────────
struct _Imu {
  int Init() { return 0; }
  // Level / face-up: no tilt, so the head faces front in renders.
  int getAccelData(float* ax, float* ay, float* az) { *ax = 0; *ay = 0; *az = 1.0f; return 0; }
};
struct _Axp {
  float GetBatVoltage() { return 4.1f; }
  float GetBatCurrent() { return 0.0f; }
  float GetVBusVoltage() { return 5.0f; }
};
struct _M5Device {
  _Imu Imu;
  _Axp Axp;
  TFT_eSPI Lcd;
  void begin(bool = true, bool = true, bool = true) {}
  void update() {}
};
extern _M5Device M5;
