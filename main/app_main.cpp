// =============================================================================
//  app_main.cpp — Arduino-style setup()/loop() on ESP-IDF
//
//  The game's main.cpp defines setup() and loop(). The task running them is the
//  one render::startWorker() pairs its column worker with, so it is pinned to
//  core 1 and the worker takes core 0, as on the Cardputer.
// =============================================================================
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

void setup();
void loop();

static void gameTask(void*) {
  setup();
  for (;;) {
    loop();
    // loop() blocks on the panel's frame-done in present(), so it never hogs
    // the core, but a tick to the scheduler keeps the IDLE task fed.
    taskYIELD();
  }
}

extern "C" void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  xTaskCreatePinnedToCore(gameTask, "game", 16384, nullptr, 5, nullptr, 1);
}
