// Host stub of Arduino.h — just enough for stats.h / buddy code to compile
// off-device for the gr0m character renderer. NOT used in the firmware build.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>

typedef unsigned char byte;
typedef bool boolean;

extern unsigned long _grender_clock_ms;          // advanced by the render harness
static inline unsigned long millis() { return _grender_clock_ms; }
static inline unsigned long micros() { return 0UL; }
static inline void delay(unsigned long) {}
static inline void yield() {}

template <typename T> static inline T _amin(T a, T b) { return a < b ? a : b; }
template <typename T> static inline T _amax(T a, T b) { return a > b ? a : b; }
#ifndef min
#define min(a, b) _amin(a, b)
#endif
#ifndef max
#define max(a, b) _amax(a, b)
#endif

// Minimal String — present only in case a header references it.
class String {
  const char* _s;
public:
  String(const char* s = "") : _s(s) {}
  const char* c_str() const { return _s; }
};
