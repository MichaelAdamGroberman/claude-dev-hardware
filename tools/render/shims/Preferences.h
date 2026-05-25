// Host stub of the Arduino Preferences (NVS) API — all no-ops returning
// defaults, so stats.h compiles off-device. NOT used in the firmware build.
#pragma once
#include <cstdint>
#include <cstddef>
#include "Arduino.h"

class Preferences {
public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  bool clear() { return true; }
  bool isKey(const char*) { return false; }
  bool remove(const char*) { return true; }

  uint8_t  getUChar (const char*, uint8_t  d = 0) { return d; }
  uint16_t getUShort(const char*, uint16_t d = 0) { return d; }
  uint32_t getUInt  (const char*, uint32_t d = 0) { return d; }
  uint32_t getULong (const char*, uint32_t d = 0) { return d; }
  int32_t  getInt   (const char*, int32_t  d = 0) { return d; }
  int      getChar  (const char*, int      d = 0) { return d; }
  bool     getBool  (const char*, bool     d = false) { return d; }
  size_t   getBytes (const char*, void*, size_t) { return 0; }
  size_t   getString(const char*, char*, size_t) { return 0; }

  size_t putUChar (const char*, uint8_t)  { return 1; }
  size_t putUShort(const char*, uint16_t) { return 1; }
  size_t putUInt  (const char*, uint32_t) { return 1; }
  size_t putULong (const char*, uint32_t) { return 1; }
  size_t putInt   (const char*, int32_t)  { return 1; }
  size_t putChar  (const char*, int)      { return 1; }
  size_t putBool  (const char*, bool)     { return 1; }
  size_t putBytes (const char*, const void*, size_t) { return 1; }
  size_t putString(const char*, const char*) { return 1; }
};
