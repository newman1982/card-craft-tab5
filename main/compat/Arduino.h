// =============================================================================
//  Arduino.h — the few Arduino calls Card Craft makes, on bare ESP-IDF
//
//  The game is written against the Arduino core (millis, micros, delay, Serial
//  for the dev build). The Tab5 port runs on ESP-IDF directly, because only the
//  IDF BSP drives the ST7121 panel; this header lets the game sources compile
//  unchanged.
// =============================================================================
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t micros() { return (uint32_t)esp_timer_get_time(); }
static inline void delay(uint32_t ms) {
  const TickType_t t = pdMS_TO_TICKS(ms);
  vTaskDelay(t ? t : 1);
}

// Serial only exists for the DEV_SERIAL instruments; printf goes to the console
// and there is never anything to read.
struct SerialShim {
  int available() { return 0; }
  int read() { return -1; }
  template <class... A>
  int printf(const char* fmt, A... a) { return ::printf(fmt, a...); }
};
extern SerialShim Serial;
