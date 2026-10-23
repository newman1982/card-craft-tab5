// =============================================================================
//  M5GFX.h — the slice of LovyanGFX that Card Craft actually touches
//
//  render.cpp owns its framebuffers and only ever hands a finished 240x135
//  frame to the panel through pushImageDMA; main.cpp prints one error string.
//  That is the whole surface. The Tab5 implementation (hal_tab5.cpp) scales the
//  frame onto the 1280x720 MIPI-DSI panel and draws the touch controls around
//  it.
// =============================================================================
#pragma once

#include <stdint.h>

namespace lgfx {
// RGB565 with its two bytes swapped, the SPI panels' wire order. The game packs
// every colour this way; the Tab5 side swaps back while scaling.
struct swap565_t {
  uint16_t raw;
};
}  // namespace lgfx

constexpr uint32_t TFT_BLACK = 0x0000;
constexpr uint32_t TFT_WHITE = 0xFFFF;
constexpr uint32_t TFT_RED   = 0xF800;

class LGFX_Device {
 public:
  void startWrite() {}
  void endWrite() {}
  void waitDMA() {}
  void pushImageDMA(int x, int y, int w, int h, const lgfx::swap565_t* data);
  void fillScreen(uint32_t rgb565);
  void setTextColor(uint32_t rgb565) { fg_ = (uint16_t)rgb565; }
  void drawString(const char* s, int x, int y);
  // Panel readback is a DEV_SERIAL instrument for SPI panels; a DPI panel has
  // nothing to read back, so this returns the frame that was last presented.
  void readRect(int x, int y, int w, int h, uint16_t* out);

 private:
  uint16_t fg_ = 0xFFFF;
};
