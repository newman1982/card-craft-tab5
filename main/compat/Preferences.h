// =============================================================================
//  Preferences.h — the Arduino Preferences API on top of NVS
//
//  Only the getters and setters the game calls. nvs_flash_init() is done in
//  app_main before setup() runs.
// =============================================================================
#pragma once

#include <stdint.h>

#include "nvs.h"

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false);
  void end();

  uint32_t getUInt(const char* key, uint32_t def = 0);
  int32_t  getInt(const char* key, int32_t def = 0);
  bool     getBool(const char* key, bool def = false);

  size_t putUInt(const char* key, uint32_t v);
  size_t putInt(const char* key, int32_t v);
  size_t putBool(const char* key, bool v);

 private:
  nvs_handle_t h_ = 0;
  bool open_ = false;
};
