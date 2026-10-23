// =============================================================================
//  usb_kbd.cpp — USB HID keyboard on the Tab5's USB-A host port
//
//  Boot protocol only: every keyboard speaks it, and its 8-byte report
//  (modifiers, reserved, six key slots) is all a game needs. Reports arrive on
//  the HID driver's task; the game reads a snapshot of held keys once a frame.
// =============================================================================
#include "tab5_io.h"

#include <string.h>

#include <atomic>

#include "bsp/m5stack_tab5.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"

namespace tab5 {

static const char* TAG = "kbd";

static KeySet s_keys;
static std::atomic<int> s_kbdCount{0};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

struct DevEvent {
  hid_host_device_handle_t dev;
  hid_host_driver_event_t  ev;
};
static QueueHandle_t s_q = nullptr;

static void onReport(const uint8_t* r, size_t len) {
  if (len < 8) return;
  KeySet k = {};
  for (int b = 0; b < 8; ++b)
    if (r[0] & (1u << b)) { const uint8_t u = 0xE0 + b; k.w[u >> 5] |= 1u << (u & 31); }
  for (int i = 2; i < 8; ++i) {
    const uint8_t u = r[i];
    if (u <= 0x03) continue;   // none / rollover error
    k.w[u >> 5] |= 1u << (u & 31);
  }
  portENTER_CRITICAL(&s_mux);
  s_keys = k;
  portEXIT_CRITICAL(&s_mux);
}

static void ifaceCb(hid_host_device_handle_t dev, const hid_host_interface_event_t ev, void*) {
  switch (ev) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
      uint8_t data[64];
      size_t len = 0;
      if (hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &len) == ESP_OK)
        onReport(data, len);
      break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "keyboard disconnected");
      hid_host_device_close(dev);
      if (s_kbdCount > 0) --s_kbdCount;
      portENTER_CRITICAL(&s_mux);
      s_keys = KeySet{};
      portEXIT_CRITICAL(&s_mux);
      break;
    default:
      break;
  }
}

// Opening a device is not allowed from inside the driver's callback, so the
// connect event is handed to a task of our own, as Espressif's example does.
static void driverCb(hid_host_device_handle_t dev, const hid_host_driver_event_t ev, void*) {
  DevEvent e = { dev, ev };
  xQueueSend(s_q, &e, 0);
}

static void connectTask(void*) {
  DevEvent e;
  for (;;) {
    if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
    if (e.ev != HID_HOST_DRIVER_EVENT_CONNECTED) continue;
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(e.dev, &p) != ESP_OK) continue;
    if (p.proto != HID_PROTOCOL_KEYBOARD) continue;   // mice etc. are ignored
    const hid_host_device_config_t cfg = { .callback = ifaceCb, .callback_arg = nullptr };
    if (hid_host_device_open(e.dev, &cfg) != ESP_OK) continue;
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
      hid_class_request_set_protocol(e.dev, HID_REPORT_PROTOCOL_BOOT);
      hid_class_request_set_idle(e.dev, 0, 0);
    }
    if (hid_host_device_start(e.dev) == ESP_OK) {
      ++s_kbdCount;
      ESP_LOGI(TAG, "keyboard connected");
    }
  }
}

bool kbdBegin() {
  if (bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true) != ESP_OK) {
    ESP_LOGW(TAG, "USB host start failed");
    return false;
  }
  s_q = xQueueCreate(4, sizeof(DevEvent));
  const hid_host_driver_config_t cfg = {
    .create_background_task = true,
    .task_priority = 5,
    .stack_size = 4096,
    .core_id = 0,
    .callback = driverCb,
    .callback_arg = nullptr,
  };
  if (hid_host_install(&cfg) != ESP_OK) {
    ESP_LOGW(TAG, "HID host install failed");
    return false;
  }
  xTaskCreatePinnedToCore(connectTask, "kbd", 4096, nullptr, 5, nullptr, 0);
  return true;
}

bool kbdConnected() { return s_kbdCount > 0; }

KeySet kbdKeys() {
  portENTER_CRITICAL(&s_mux);
  const KeySet k = s_keys;
  portEXIT_CRITICAL(&s_mux);
  return k;
}

}  // namespace tab5
