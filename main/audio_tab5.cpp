// =============================================================================
//  audio_tab5.cpp — the Cardputer Speaker's job, done on the Tab5's ES8388
//
//  The game asks for two things (see hal.h): a wavetable voice on a channel,
//  and a block of 8-bit signed PCM on a channel. M5Unified mixes eight such
//  channels on the Cardputer; this does the same, in a task that feeds the
//  codec 48 kHz mono in small blocks so a new cue is heard within ~5 ms.
// =============================================================================
#include "tab5_io.h"

#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tab5 {

static const char* TAG = "audio";

constexpr uint32_t RATE  = 48000;
constexpr int      NCH   = 8;
constexpr int      BLOCK = 256;   // frames per write, 5.3 ms

// The same single-cycle tables the Cardputer HAL hands to Speaker::tone, here
// centred on zero.
static const int8_t kSquare[16] = { 127,127,127,127,127,127,127,127,
                                    -128,-128,-128,-128,-128,-128,-128,-128 };
static const int8_t kSaw[16]    = { -128,-111,-94,-77,-60,-43,-26,-9,
                                    8,25,42,59,76,93,110,127 };
static const int8_t kTri[16]    = { -128,-96,-64,-32,0,32,64,96,
                                    127,96,64,32,0,-32,-64,-96 };
static const int8_t kNoise[16]  = { -111,75,-67,122,-120,16,103,-36,
                                    50,-93,127,-9,-124,39,-52,84 };
static const int8_t* const kWave[4] = { kSquare, kSaw, kTri, kNoise };

struct Chan {
  const int8_t* data = nullptr;  // wavetable (len 16, looped) or PCM (one shot)
  uint32_t len = 0;              // in samples
  uint32_t pos = 0;              // 16.16 fixed point
  uint32_t step = 0;             // 16.16 per output frame
  uint32_t left = 0;             // frames still to play (voices only)
  bool     loop = false;
  uint8_t  vol = 0;
};

static Chan s_ch[NCH];
// Bumped by every request, so the mixer can tell whether a channel it advanced
// was replaced by the game while it was mixing.
static uint32_t s_gen[NCH];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_codec_dev_handle_t s_spk = nullptr;

static void audioTask(void*) {
  static int16_t out[BLOCK];
  for (;;) {
    Chan ch[NCH];
    uint32_t gen[NCH];
    portENTER_CRITICAL(&s_mux);
    memcpy(ch, s_ch, sizeof(ch));
    memcpy(gen, s_gen, sizeof(gen));
    portEXIT_CRITICAL(&s_mux);

    int32_t acc[BLOCK] = {};
    for (int c = 0; c < NCH; ++c) {
      Chan& k = ch[c];
      if (!k.data || !k.vol) continue;
      for (int i = 0; i < BLOCK; ++i) {
        uint32_t idx = k.pos >> 16;
        if (k.loop) {
          if (k.left == 0) { k.data = nullptr; break; }
          --k.left;
          idx &= 15;
        } else if (idx >= k.len) {
          k.data = nullptr;
          break;
        }
        acc[i] += (int32_t)k.data[idx] * k.vol;
        k.pos += k.step;
      }
    }
    // Eight channels at full volume would be 8 * 127 * 255; the master gain is
    // set so two or three loud cues together still stay clear of clipping.
    for (int i = 0; i < BLOCK; ++i) {
      int32_t v = acc[i] >> 2;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      out[i] = (int16_t)v;
    }

    // Keep our progress, unless the game replaced the channel meanwhile.
    portENTER_CRITICAL(&s_mux);
    for (int c = 0; c < NCH; ++c)
      if (s_gen[c] == gen[c]) s_ch[c] = ch[c];
    portEXIT_CRITICAL(&s_mux);

    esp_codec_dev_write(s_spk, out, sizeof(out));
  }
}

bool audioBegin() {
  s_spk = bsp_audio_codec_speaker_init();
  if (!s_spk) {
    ESP_LOGW(TAG, "speaker codec not found, running silent");
    return false;
  }
  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 1;
  fs.sample_rate = RATE;
  if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "speaker open failed, running silent");
    s_spk = nullptr;
    return false;
  }
  esp_codec_dev_set_out_vol(s_spk, 65);
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 6, nullptr, 0);
  return true;
}

void audioVoice(uint8_t ch, uint16_t freqHz, uint16_t ms, uint8_t wave, uint8_t vol) {
  if (!s_spk) return;
  if (ch >= NCH) ch = NCH - 1;
  if (wave > 3) wave = 3;
  Chan k;
  k.data = kWave[wave];
  k.len  = 16;
  k.loop = true;
  k.step = (uint32_t)(((uint64_t)freqHz * 16u << 16) / RATE);
  k.left = (uint32_t)ms * (RATE / 1000);
  k.vol  = vol;
  portENTER_CRITICAL(&s_mux);
  s_ch[ch] = k;
  ++s_gen[ch];
  portEXIT_CRITICAL(&s_mux);
}

bool audioSample(uint8_t ch, const int8_t* pcm, size_t n, uint32_t rateHz, uint8_t vol) {
  if (!s_spk) return false;
  if (ch >= NCH) ch = NCH - 1;
  Chan k;
  k.data = pcm;
  k.len  = (uint32_t)n;
  k.loop = false;
  k.step = (uint32_t)(((uint64_t)rateHz << 16) / RATE);
  k.vol  = vol;
  portENTER_CRITICAL(&s_mux);
  s_ch[ch] = k;
  ++s_gen[ch];
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void audioSilence() {
  portENTER_CRITICAL(&s_mux);
  for (int c = 0; c < NCH; ++c) { s_ch[c] = Chan(); ++s_gen[c]; }
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace tab5
