// =============================================================================
//  tab5_kbd.cpp — M5Stack Tab5 Keyboard (SKU A164) on the expansion connector
//
//  A 70-key matrix behind an STM32 at I2C 0x6D (SDA GPIO0, SCL GPIO1), on its
//  own bus: the BSP owns I2C1 for the board's internal devices, so this takes
//  I2C0. Register protocol as documented by M5Stack's M5Tab5-Keyboard-UserDemo
//  (MIT):
//
//    0x01 INT_STA     bit0 = key events pending (normal mode); write 0 clears
//    0x02 EVENT_NUM   events queued
//    0x10 MODE        0 = normal (row/col with press AND release)
//    0x20 KEY_EVENT   one event per read: bit7 pressed, bits6-4 row, 3-0 col
//    0xFE VERSION
//
//  Normal mode rather than the module's HID mode: HID mode reports a keycode
//  per press but no release, and the game needs to know a key is still held.
//  Keys are translated to HID usage IDs so the game-side mapping in
//  hal_tab5.cpp serves this keyboard and a USB one alike.
// =============================================================================
#include "tab5_io.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tab5 {

static const char* TAG = "tab5kbd";

constexpr uint8_t ADDR = 0x6D;
constexpr int SDA = 0, SCL = 1;
constexpr uint8_t REG_INT_STA = 0x01, REG_EVENT_NUM = 0x02, REG_MODE = 0x10;
constexpr uint8_t REG_KEY_EVENT = 0x20, REG_VERSION = 0xFE;

// Key id = row * 14 + col, in the module's own numbering, to HID usage.
// 0 where the key has no usage the game could want (SYM, Aa, CTRL, ALT...).
static const uint8_t kUsage[70] = {
  // row 0: ESC 1 2 3 4 5 6 7 8 9 0 - + DEL
  0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x4C,
  // row 1: ` ! @ # $ % ^ & * ( ) [ ] backslash
  0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x2F, 0x30, 0x31,
  // row 2: TAB q w e r t y u i o p ; ' BS
  0x2B, 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x33, 0x34, 0x2A,
  // row 3: SYM Aa a s d f g h j k l UP _ ENT
  0, 0, 0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x52, 0, 0x28,
  // row 4: CTRL ALT z x c v b n m . LEFT DOWN RIGHT SPACE
  0xE0, 0xE2, 0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10, 0x37, 0x50, 0x51, 0x4F, 0x2C,
};

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static KeySet s_keys;
static bool s_present = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool rd(uint8_t reg, uint8_t* v) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 20) == ESP_OK;
}

static bool wr(uint8_t reg, uint8_t v) {
  const uint8_t b[2] = { reg, v };
  return i2c_master_transmit(s_dev, b, 2, 20) == ESP_OK;
}

static bool probe() {
  if (i2c_master_probe(s_bus, ADDR, 20) != ESP_OK) return false;
  uint8_t ver = 0;
  rd(REG_VERSION, &ver);
  if (!wr(REG_MODE, 0)) return false;   // normal mode
  wr(REG_INT_STA, 0);
  ESP_LOGI(TAG, "Tab5 Keyboard found, FW 0x%02X", ver);
  return true;
}

static void setKey(KeySet& k, uint8_t usage, bool on) {
  const uint32_t bit = 1u << (usage & 31);
  if (on) k.w[usage >> 5] |= bit;
  else    k.w[usage >> 5] &= ~bit;
}

static void pollTask(void*) {
  KeySet k = {};
  int fails = 0;
  for (;;) {
    if (!s_present) {
      // Hot-plug: the module is on a connector and can arrive at any time.
      if (probe()) {
        s_present = true;
        fails = 0;
        k = KeySet{};
      } else {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }

    uint8_t sta = 0, n = 0;
    if (!rd(REG_INT_STA, &sta)) {
      if (++fails > 20) {
        ESP_LOGI(TAG, "Tab5 Keyboard gone");
        s_present = false;
        portENTER_CRITICAL(&s_mux);
        s_keys = KeySet{};
        portEXIT_CRITICAL(&s_mux);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    fails = 0;

    if ((sta & 0x01) && rd(REG_EVENT_NUM, &n)) {
      if (n > 32) n = 32;
      for (int i = 0; i < n; ++i) {
        uint8_t ev = 0xFF;
        if (!rd(REG_KEY_EVENT, &ev) || ev == 0xFF) break;
        const int row = (ev >> 4) & 0x07, col = ev & 0x0F;
        const int id = row * 14 + col;
        if (row > 4 || col > 13) continue;
        const uint8_t u = kUsage[id];
        if (u) setKey(k, u, (ev & 0x80) != 0);
      }
      wr(REG_INT_STA, 0);
      portENTER_CRITICAL(&s_mux);
      s_keys = k;
      portEXIT_CRITICAL(&s_mux);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool tab5KbdBegin() {
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_0;
  bc.sda_io_num = (gpio_num_t)SDA;
  bc.scl_io_num = (gpio_num_t)SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {
    ESP_LOGW(TAG, "I2C0 bus init failed");
    return false;
  }
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = ADDR;
  dc.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) return false;
  xTaskCreatePinnedToCore(pollTask, "tab5kbd", 3072, nullptr, 5, nullptr, 0);
  return true;
}

bool tab5KbdConnected() { return s_present; }

KeySet tab5KbdKeys() {
  portENTER_CRITICAL(&s_mux);
  const KeySet k = s_keys;
  portEXIT_CRITICAL(&s_mux);
  return k;
}

}  // namespace tab5
