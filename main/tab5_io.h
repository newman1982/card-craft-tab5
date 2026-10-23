// =============================================================================
//  tab5_io.h — the Tab5 board pieces hal_tab5.cpp is assembled from
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tab5 {

// ---- audio_tab5.cpp: an 8-channel mixer into the ES8388 speaker -------------
bool audioBegin();
void audioVoice(uint8_t ch, uint16_t freqHz, uint16_t ms, uint8_t wave, uint8_t vol);
bool audioSample(uint8_t ch, const int8_t* pcm, size_t n, uint32_t rateHz, uint8_t vol);
void audioSilence();

// ---- usb_kbd.cpp: USB HID keyboard on the USB-A host port -------------------
// Keys are HID usage IDs (0x04 = 'a' ... see the USB HID Usage Tables). The
// modifier byte is folded in as usages 0xE0..0xE7, as the spec numbers them.
bool kbdBegin();
bool kbdConnected();
// Snapshot of every key currently held, one bit per usage ID.
struct KeySet {
  uint32_t w[8];
  bool has(uint8_t usage) const { return (w[usage >> 5] >> (usage & 31)) & 1u; }
};
KeySet kbdKeys();

// ---- tab5_kbd.cpp: M5Stack Tab5 Keyboard module (I2C 0x6D) ------------------
// Reports in the same HID usage IDs as the USB keyboard.
bool tab5KbdBegin();
bool tab5KbdConnected();
KeySet tab5KbdKeys();

}  // namespace tab5
