#include "Arduino.h"
#include "Preferences.h"

SerialShim Serial;

bool Preferences::begin(const char* name, bool readOnly) {
  end();
  open_ = nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE, &h_) == ESP_OK;
  return open_;
}

void Preferences::end() {
  if (open_) nvs_close(h_);
  open_ = false;
}

uint32_t Preferences::getUInt(const char* key, uint32_t def) {
  uint32_t v = def;
  if (open_ && nvs_get_u32(h_, key, &v) != ESP_OK) v = def;
  return v;
}

int32_t Preferences::getInt(const char* key, int32_t def) {
  int32_t v = def;
  if (open_ && nvs_get_i32(h_, key, &v) != ESP_OK) v = def;
  return v;
}

bool Preferences::getBool(const char* key, bool def) {
  uint8_t v = def;
  if (open_ && nvs_get_u8(h_, key, &v) != ESP_OK) v = def;
  return v != 0;
}

size_t Preferences::putUInt(const char* key, uint32_t v) {
  if (!open_ || nvs_set_u32(h_, key, v) != ESP_OK) return 0;
  nvs_commit(h_);
  return sizeof(v);
}

size_t Preferences::putInt(const char* key, int32_t v) {
  if (!open_ || nvs_set_i32(h_, key, v) != ESP_OK) return 0;
  nvs_commit(h_);
  return sizeof(v);
}

size_t Preferences::putBool(const char* key, bool v) {
  if (!open_ || nvs_set_u8(h_, key, v ? 1 : 0) != ESP_OK) return 0;
  nvs_commit(h_);
  return 1;
}
