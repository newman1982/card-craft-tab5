// =============================================================================
//  hal_tab5.cpp — M5Stack Tab5 (ESP32-P4), ST7121 panel
//
//  Display: the 720x1280 MIPI-DSI panel is used in landscape. The game's
//  240x135 frame is scaled 4x to 960x540 at the top centre; the strips left,
//  right and below it carry the touch controls. The DPI driver owns two
//  framebuffers in PSRAM; present() scales straight into the one not on screen
//  and flips, so there is no intermediate copy.
//
//  Input: the touch controls, plus a keyboard -- the Tab5 Keyboard module
//  (tab5_kbd.cpp) or a USB one on the USB-A port -- with the Cardputer's layout (arrows move, W mines, A builds, E/S look, SPACE jumps,
//  TAB crafts, ENTER confirms, ESC backs out, Q drops, 1-9 pick a slot). Both
//  work at once; whatever is held on either counts.
//
//  Only the ST7121 panel revision is targeted: the DSI lane rate below is the
//  one the BSP uses for ST712x. The BSP itself still autodetects the board.
// =============================================================================
#if defined(BOARD_TAB5)

#include "hal/hal.h"

#include <string.h>

#include "esp_lcd_touch.h"
#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "font5x7.h"
#include "tab5_io.h"

// Landscape orientation; 1 is the right way up on the ST7121 Tab5. Touch
// follows the same transform, so flipping this keeps them in agreement.
#ifndef TAB5_ROTATE_180
#define TAB5_ROTATE_180 1
#endif

static const char* TAG = "hal_tab5";

// ---- geometry ----------------------------------------------------------------

constexpr int PW = 720, PH = 1280;        // panel, native portrait
constexpr int LW = 1280, LH = 720;        // landscape as the player sees it
constexpr int GW = 240, GH = 135;         // the game's frame
// Touch layout: the view at 4x, top centre, controls in the strips around it.
constexpr int SCALE = 4;
constexpr int GX0 = (LW - GW * SCALE) / 2;  // 160
constexpr int GY0 = 0;
// Keyboard layout: no controls on screen, so the view fills the panel. The
// game is 16:9 like the panel, so this is a uniform 16/3 scale -- nearest
// neighbour, game pixels alternating 5 and 6 panel pixels wide.

// Landscape (lx, ly) -> panel (px, py).
static inline int toPx(int lx, int ly) { (void)lx; return TAB5_ROTATE_180 ? ly : PW - 1 - ly; }
static inline int toPy(int lx, int ly) { (void)ly; return TAB5_ROTATE_180 ? PH - 1 - lx : lx; }
// Panel touch (x, y) -> landscape.
static inline void fromPanel(int x, int y, int& lx, int& ly) {
  if (TAB5_ROTATE_180) { lx = PH - 1 - y; ly = x; }
  else                 { lx = y;          ly = PW - 1 - x; }
}

// ---- panel -------------------------------------------------------------------

static esp_lcd_panel_handle_t s_panel = nullptr;
static uint16_t* s_fb[2] = { nullptr, nullptr };
static int s_back = 1;                        // fb[0] is on screen after init
static SemaphoreHandle_t s_frameDone = nullptr;
static uint16_t s_last[GW * GH];              // last presented frame, swapped
static int s_ctrlDirty = 2;                   // redraw controls into N buffers
static int s_clearDirty = 0;                  // clear the whole screen in N buffers
static bool s_kbdLayout = false;              // keyboard attached: big view, no controls
static int s_vx0 = GX0, s_vy0 = GY0, s_vw = GW * SCALE, s_vh = GH * SCALE;
static LGFX_Device s_dev;

static bool IRAM_ATTR onFrameDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR(s_frameDone, &woke);
  return woke == pdTRUE;
}

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Fills a landscape rectangle, clipped to the screen.
static void lrect(uint16_t* fb, int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LW) w = LW - x;
  if (y + h > LH) h = LH - y;
  if (w <= 0 || h <= 0) return;
  for (int lx = x; lx < x + w; ++lx) {
    const int py = toPy(lx, 0);
    const int pa = toPx(lx, y), pb = toPx(lx, y + h - 1);
    const int p0 = pa < pb ? pa : pb;
    uint16_t* row = fb + py * PW + p0;
    for (int i = 0; i < h; ++i) row[i] = c;
  }
}

static void lframe(uint16_t* fb, int x, int y, int w, int h, int t, uint16_t c) {
  lrect(fb, x, y, w, t, c);
  lrect(fb, x, y + h - t, w, t, c);
  lrect(fb, x, y, t, h, c);
  lrect(fb, x + w - t, y, t, h, c);
}

// The game's own 5x7 HUD font, so the controls read in the same type as the
// hints that name them. Uppercase, digits and the four arrows.
static int ltextW(const char* s, int sc) {
  const int n = (int)strlen(s);
  return n ? n * (FONT_W + 1) * sc - sc : 0;
}

static void ltext(uint16_t* fb, int x, int y, const char* s, int sc, uint16_t c) {
  for (; *s; ++s, x += (FONT_W + 1) * sc) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < FONT_FIRST || ch > FONT_LAST) continue;
    const uint8_t* g = kFont5x7[ch - FONT_FIRST];
    for (int cx = 0; cx < FONT_W; ++cx)
      for (int cy = 0; cy < FONT_H; ++cy)
        if (g[cx] & (1u << cy)) lrect(fb, x + cx * sc, y + cy * sc, sc, sc, c);
  }
}

static void flip() {
  // Clear any stale "done" so the next wait is for a frame that started after
  // this flip, i.e. one that scans the buffer we just finished.
  xSemaphoreTake(s_frameDone, 0);
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PW, PH, s_fb[s_back]);
  s_back ^= 1;
}

static void waitBack() {
  // The buffer we are about to draw into was on screen until the flip; it is
  // free once the panel has begun a frame from the other one.
  xSemaphoreTake(s_frameDone, pdMS_TO_TICKS(40));
}

// ---- touch controls -----------------------------------------------------------

enum BtnId : uint8_t {
  B_ESC, B_CRAFT, B_DROP, B_OK,
  B_LUP, B_CTR, B_LDN,
  B_FL, B_FWD, B_FR, B_LEFT, B_BACK, B_RIGHT,
  B_MINE, B_BUILD, B_JUMP,
  B_S1, B_S2, B_S3, B_S4, B_S5, B_S6, B_S7, B_S8, B_S9,
  B_COUNT
};

struct Btn {
  int16_t x, y, w, h;
  const char* label;
  uint8_t r, g, b;   // base tint
};

constexpr int BY1 = GY0 + GH * SCALE;   // 540: top of the bottom strip
constexpr int BRH = (LH - BY1) / 2;     // 90: one row of it

// Left column: menu keys. Right column: looking. Bottom: D-pad (inverted T,
// with the corners as forward-diagonals), hotbar, and the action keys under
// the right thumb.
static const Btn kBtn[B_COUNT] = {
  /*ESC  */ {   0,   0, GX0, 135, "ESC",   90, 50, 50 },
  /*CRAFT*/ {   0, 135, GX0, 135, "CRAFT", 70, 60, 40 },
  /*DROP */ {   0, 270, GX0, 135, "DROP",  60, 60, 60 },
  /*OK   */ {   0, 405, GX0, 135, "OK",    40, 80, 50 },
  /*LUP  */ { GX0 + GW * SCALE,   0, GX0, 180, "LOOK\x18", 50, 60, 80 },
  /*CTR  */ { GX0 + GW * SCALE, 180, GX0, 180, "CTR",      50, 50, 60 },
  /*LDN  */ { GX0 + GW * SCALE, 360, GX0, 180, "LOOK\x19", 50, 60, 80 },
  /*FL   */ {   0, BY1,       150, BRH, "\x1B\x18", 45, 45, 55 },
  /*FWD  */ { 150, BY1,       150, BRH, "\x18",     60, 60, 75 },
  /*FR   */ { 300, BY1,       150, BRH, "\x18\x1A", 45, 45, 55 },
  /*LEFT */ {   0, BY1 + BRH, 150, BRH, "\x1B",     60, 60, 75 },
  /*BACK */ { 150, BY1 + BRH, 150, BRH, "\x19",     60, 60, 75 },
  /*RIGHT*/ { 300, BY1 + BRH, 150, BRH, "\x1A",     60, 60, 75 },
  /*MINE */ {  830, BY1,       225, BRH, "MINE",  95, 55, 35 },
  /*BUILD*/ { 1055, BY1,       225, BRH, "BUILD", 40, 75, 45 },
  /*JUMP */ {  830, BY1 + BRH, 450, BRH, "JUMP",  50, 65, 90 },
  /*S1-5 */ { 450, BY1, 76, BRH, "1", 55, 55, 55 }, { 526, BY1, 76, BRH, "2", 55, 55, 55 },
            { 602, BY1, 76, BRH, "3", 55, 55, 55 }, { 678, BY1, 76, BRH, "4", 55, 55, 55 },
            { 754, BY1, 76, BRH, "5", 55, 55, 55 },
  /*S6-9 */ { 450, BY1 + BRH, 95, BRH, "6", 55, 55, 55 }, { 545, BY1 + BRH, 95, BRH, "7", 55, 55, 55 },
            { 640, BY1 + BRH, 95, BRH, "8", 55, 55, 55 }, { 735, BY1 + BRH, 95, BRH, "9", 55, 55, 55 },
};

static esp_lcd_touch_handle_t s_tp = nullptr;
static uint32_t s_touchHeld = 0;   // bit per BtnId
static uint32_t s_drawnHeld = ~0u;

static void drawControls(uint16_t* fb) {
  for (int i = 0; i < B_COUNT; ++i) {
    const Btn& b = kBtn[i];
    const bool on = s_touchHeld & (1u << i);
    const int k = on ? 2 : 1;
    const uint16_t face = rgb(b.r * k > 255 ? 255 : b.r * k,
                              b.g * k > 255 ? 255 : b.g * k,
                              b.b * k > 255 ? 255 : b.b * k);
    lrect(fb, b.x, b.y, b.w, b.h, rgb(0, 0, 0));
    lrect(fb, b.x + 3, b.y + 3, b.w - 6, b.h - 6, face);
    lframe(fb, b.x + 3, b.y + 3, b.w - 6, b.h - 6, 2, on ? rgb(255, 255, 255) : rgb(120, 120, 130));
    const int sc = (b.w < 100) ? 4 : 3;
    const int tw = ltextW(b.label, sc);
    ltext(fb, b.x + (b.w - tw) / 2, b.y + (b.h - FONT_H * sc) / 2, b.label, sc,
          rgb(235, 235, 235));
  }
  // A keyboard hint under the D-pad corner would crowd it; the frame around
  // the game view is enough to show where the picture ends.
  lframe(fb, GX0 - 2, GY0, GW * SCALE + 4, GH * SCALE + 2, 2, rgb(30, 30, 36));
}

static void pollTouch() {
  if (!s_tp) return;
  if (s_kbdLayout) { s_touchHeld = 0; return; }   // nothing on screen to touch
  uint32_t held = 0;
  if (esp_lcd_touch_read_data(s_tp) == ESP_OK) {
    esp_lcd_touch_point_data_t pts[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_tp, pts, &n, CONFIG_ESP_LCD_TOUCH_MAX_POINTS) == ESP_OK) {
      for (int p = 0; p < n; ++p) {
        int lx, ly;
        fromPanel(pts[p].x, pts[p].y, lx, ly);
        for (int i = 0; i < B_COUNT; ++i) {
          const Btn& b = kBtn[i];
          if (lx >= b.x && lx < b.x + b.w && ly >= b.y && ly < b.y + b.h) held |= 1u << i;
        }
      }
    }
  }
  s_touchHeld = held;
  if (held != s_drawnHeld) { s_drawnHeld = held; s_ctrlDirty = 2; }
}

// ---- LGFX_Device shim --------------------------------------------------------

void LGFX_Device::pushImageDMA(int x, int y, int w, int h, const lgfx::swap565_t* data) {
  (void)x; (void)y;
  if (w != GW || h != GH) return;
  memcpy(s_last, data, sizeof(s_last));
  waitBack();
  uint16_t* fb = s_fb[s_back];
  const uint16_t* src = (const uint16_t*)data;

  // One game column becomes four panel rows: build the 540-pixel run once in
  // panel order, swap the bytes back to native RGB565 on the way, copy it 4x.
  // Switch layouts when a keyboard comes or goes. Both buffers have to be
  // repainted from scratch, so the change takes two frames to settle.
  const bool kbd = tab5::kbdConnected() || tab5::tab5KbdConnected();
  if (kbd != s_kbdLayout) {
    s_kbdLayout = kbd;
    s_vx0 = kbd ? 0 : GX0;
    s_vy0 = kbd ? 0 : GY0;
    s_vw  = kbd ? LW : GW * SCALE;
    s_vh  = kbd ? LH : GH * SCALE;
    s_clearDirty = 2;
    s_ctrlDirty  = kbd ? 0 : 2;
    s_touchHeld  = 0;
  }
  if (s_clearDirty > 0) {
    const uint16_t bg = rgb(12, 12, 16);
    for (int p = 0; p < PW * PH; ++p) fb[p] = bg;
    --s_clearDirty;
  }

  // Each game column becomes a few panel rows: build the run once in panel
  // order, swapping the bytes back to native RGB565 on the way, then copy it to
  // every panel row that samples the same column.
  const int vx0 = s_vx0, vy0 = s_vy0, vw = s_vw, vh = s_vh;
  uint16_t seg[LH];
  const int pa = toPx(0, vy0), pb = toPx(0, vy0 + vh - 1);
  const int p0 = pa < pb ? pa : pb;
  int built = -1;
  for (int x = 0; x < vw; ++x) {
    const int gx = x * GW / vw;
    if (gx != built) {
      for (int i = 0; i < vh; ++i) {
        const int ry = TAB5_ROTATE_180 ? i : vh - 1 - i;
        const uint16_t v = src[(ry * GH / vh) * GW + gx];
        seg[i] = (uint16_t)((v >> 8) | (v << 8));
      }
      built = gx;
    }
    memcpy(fb + toPy(vx0 + x, 0) * PW + p0, seg, vh * sizeof(uint16_t));
  }
  if (s_ctrlDirty > 0 && !s_kbdLayout) { drawControls(fb); --s_ctrlDirty; }
  flip();
}

void LGFX_Device::fillScreen(uint32_t c) {
  for (int i = 0; i < 2; ++i) {
    uint16_t* fb = s_fb[i];
    for (int p = 0; p < PW * PH; ++p) fb[p] = (uint16_t)c;
    esp_cache_msync(fb, PW * PH * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
}

void LGFX_Device::drawString(const char* s, int x, int y) {
  // Only the boot-failure message uses this; draw it into both buffers so it
  // stays up whichever one is showing.
  for (int i = 0; i < 2; ++i) {
    ltext(s_fb[i], x * SCALE, y * SCALE, s, 4, fg_);
    esp_cache_msync(s_fb[i], PW * PH * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
}

void LGFX_Device::readRect(int x, int y, int w, int h, uint16_t* out) {
  for (int r = 0; r < h; ++r)
    for (int c = 0; c < w; ++c) {
      const uint16_t v = s_last[(y + r) * GW + x + c];
      out[r * w + c] = (uint16_t)((v >> 8) | (v << 8));
    }
}

// ---- hal ----------------------------------------------------------------------

namespace hal {

static Buttons s_btn;
static Buttons s_prev;
static bool s_prevEnter = false, s_prevPause = false, s_prevCraft = false;
static uint16_t s_prevSlot = 0;

// BUILD is paced here rather than in the game. The simulation places a block
// every BUILD_TICKS (~150 ms) for as long as the key is held, which is how the
// Cardputer plays it, but a finger on a touch key or a key travel on a real
// keyboard is long enough that one press lays two or three. So the board hands
// the game a short pulse: one block on the press, then -- only if the key is still
// down after a typing-style delay -- a slower repeat for walling up.
constexpr uint32_t BUILD_DELAY_MS  = 450;
constexpr uint32_t BUILD_REPEAT_MS = 250;
// How long each pulse stays up. Long enough to ride out a swing's cooldown that
// was still running when the key went down, shorter than the game's own
// BUILD_TICKS (150 ms) after a placement, so a pulse can never lay two.
constexpr uint32_t BUILD_PULSE_MS  = 140;
static uint32_t s_buildUntil = 0;
static bool     s_buildRaw = false;
static uint32_t s_buildNext = 0;

static const Caps s_capsTouch = {
  /*kTurn   */ "\x1B\x1A",
  /*kMove   */ "\x18\x19",
  /*kAct    */ "MINE",
  /*kBuild  */ "BUILD",
  /*kConfirm*/ "OK",
  /*kBack   */ "ESC",
  /*kCraft  */ "CRAFT",
  /*kDrop   */ "DROP",
  /*kLook   */ "LOOK",
  /*kCentre */ "CTR",
  /*kJump   */ "JUMP",
};

// With a keyboard plugged in, the hints name the keys, as on the Cardputer.
static const Caps s_capsKbd = {
  /*kTurn   */ "\x1B\x1A",
  /*kMove   */ "\x18\x19",
  /*kAct    */ "W",
  /*kBuild  */ "A",
  /*kConfirm*/ "ENTER",
  /*kBack   */ "ESC",
  /*kCraft  */ "TAB",
  /*kDrop   */ "Q",
  /*kLook   */ "E S",
  /*kCentre */ "ENTER",
  /*kJump   */ "SPACE",
};

void begin() {
  bsp_display_config_t cfg = {};
  cfg.dsi_bus.phy_clk_src = (mipi_dsi_phy_clock_source_t)0;
  cfg.dsi_bus.lane_bit_rate_mbps = 965;   // BSP_LCD_ST712X_MIPI_DSI_LANE_BITRATE_MBPS
  esp_lcd_panel_io_handle_t io = nullptr;
  ESP_ERROR_CHECK(bsp_display_new(&cfg, &s_panel, &io));
  esp_lcd_panel_disp_on_off(s_panel, true);

  void* f0 = nullptr;
  void* f1 = nullptr;
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &f0, &f1));
  s_fb[0] = (uint16_t*)f0;
  s_fb[1] = (uint16_t*)f1;

  s_frameDone = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_frame_buf_complete = onFrameDone;
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, nullptr));

  s_dev.fillScreen(rgb(12, 12, 16));
  drawControls(s_fb[0]);
  esp_cache_msync(s_fb[0], PW * PH * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  bsp_display_brightness_set(80);

  if (bsp_touch_new(nullptr, &s_tp) != ESP_OK) {
    ESP_LOGW(TAG, "touch init failed, keyboard only");
    s_tp = nullptr;
  }
  tab5::audioBegin();
  tab5::tab5KbdBegin();
  tab5::kbdBegin();
}

LGFX_Device& display() { return s_dev; }

void update() {
  pollTouch();
  const uint32_t t = s_touchHeld;
  auto T = [t](BtnId id) { return (t & (1u << id)) != 0; };

  tab5::KeySet k = tab5::kbdKeys();
  const tab5::KeySet m = tab5::tab5KbdKeys();
  for (int i = 0; i < 8; ++i) k.w[i] |= m.w[i];
  // HID usage IDs: letters from 0x04 ('a'), digits 1..9 from 0x1E.
  auto K = [&k](uint8_t u) { return k.has(u); };
  constexpr uint8_t kA = 0x04, kE = 0x08, kQ = 0x14, kS = 0x16, kW = 0x1A;
  constexpr uint8_t kEnter = 0x28, kEsc = 0x29, kTab = 0x2B, kSpace = 0x2C;
  constexpr uint8_t kGrave = 0x35, kRight = 0x4F, kLeft = 0x50, kDown = 0x51, kUp = 0x52;
  constexpr uint8_t kPadEnter = 0x58;

  s_prev = s_btn;
  Buttons b;
  b.left  = T(B_LEFT) || T(B_FL) || K(kLeft);
  b.right = T(B_RIGHT) || T(B_FR) || K(kRight);
  b.fwd   = T(B_FWD) || T(B_FL) || T(B_FR) || K(kUp);
  b.back  = T(B_BACK) || K(kDown);
  b.act   = T(B_MINE) || K(kW);
  const bool buildRaw = T(B_BUILD) || K(kA);
  const uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
  if (buildRaw && !s_buildRaw) {
    s_buildUntil = nowMs + BUILD_PULSE_MS;
    s_buildNext  = nowMs + BUILD_DELAY_MS;
  } else if (buildRaw && (int32_t)(nowMs - s_buildNext) >= 0) {
    s_buildUntil = nowMs + BUILD_PULSE_MS;
    s_buildNext  = nowMs + BUILD_REPEAT_MS;
  }
  b.build = buildRaw && (int32_t)(nowMs - s_buildUntil) < 0;
  b.drop  = T(B_DROP) || K(kQ);
  b.lookUp   = T(B_LUP) || K(kE);
  b.lookDown = T(B_LDN) || K(kS);
  const bool enter = T(B_OK) || K(kEnter) || K(kPadEnter);
  b.lookCentre = enter || T(B_CTR);
  b.jump = T(B_JUMP) || K(kSpace);

  const bool pause = T(B_ESC) || K(kEsc) || K(kGrave);
  const bool crf   = T(B_CRAFT) || K(kTab);

  b.leftEdge  = b.left  && !s_prev.left;
  b.rightEdge = b.right && !s_prev.right;
  b.fwdEdge   = b.fwd   && !s_prev.fwd;
  b.backEdge  = b.back  && !s_prev.back;
  b.actEdge   = b.act   && !s_prev.act;
  b.buildEdge = buildRaw && !s_buildRaw;
  s_buildRaw  = buildRaw;
  b.confirmEdge = enter && !s_prevEnter;
  b.cancelEdge  = pause && !s_prevPause;
  b.navUp    = b.fwdEdge;
  b.navDown  = b.backEdge;
  b.navLeft  = b.leftEdge;
  b.navRight = b.rightEdge;
  b.craftEdge = crf && !s_prevCraft;

  uint16_t slotNow = 0;
  for (int i = 0; i < 9; ++i)
    if (T((BtnId)(B_S1 + i)) || K((uint8_t)(0x1E + i))) slotNow |= (uint16_t)(1u << i);
  for (int i = 0; i < 9; ++i) {
    const uint16_t bit = (uint16_t)(1u << i);
    if ((slotNow & bit) && !(s_prevSlot & bit)) { b.slotPick = (uint8_t)(i + 1); break; }
  }
  s_prevSlot = slotNow;

  s_prevEnter = enter;
  s_prevPause = pause;
  s_prevCraft = crf;
  s_btn = b;
}

const Buttons& buttons() { return s_btn; }
const Caps& caps() { return (tab5::kbdConnected() || tab5::tab5KbdConnected()) ? s_capsKbd : s_capsTouch; }
const char* boardName() { return "TAB5"; }

void beep(uint16_t freqHz, uint16_t ms) { tab5::audioVoice(0, freqHz, ms, 0, 200); }

void voice(uint8_t channel, uint16_t freqHz, uint16_t ms, uint8_t wave, uint8_t vol) {
  tab5::audioVoice(channel, freqHz, ms, wave, vol);
}

bool sample(uint8_t channel, const int8_t* pcm, size_t n, uint32_t rateHz, uint8_t vol) {
  return tab5::audioSample(channel, pcm, n, rateHz, vol);
}

void silence() { tab5::audioSilence(); }

}  // namespace hal

#endif  // BOARD_TAB5
