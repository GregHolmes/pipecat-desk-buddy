#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "human_face_detect.hpp"
#include "main.h"

// On-device face tracking: GC0308 camera -> esp-dl human_face_detect, a few
// times a second on a low-priority task. Publishes where the face is so the
// display task can point the eyes at it. Nothing leaves the device.

#define FACE_TRACK_INTERVAL_MS 400
#define PRESENCE_LINES 0
// Detection misses frames now and then; hold the last position this long.
#define FACE_LOST_MS 5000
// Default 0.5 per stage misses too many frames with this small sensor.
#define FACE_SCORE_THR 0.3f
#define FACE_TRACK_LOG_MS 2000

// Debug: show the camera image (with detection boxes) instead of the face.
#ifndef FACE_TRACK_PREVIEW
#define FACE_TRACK_PREVIEW 0
#endif

volatile int pipecat_face_target_x = 0;  // -100 (left) .. 100 (right)
volatile int pipecat_face_target_y = 0;  // -100 (up) .. 100 (down)
volatile bool pipecat_face_present = false;

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static void face_track_task(void *) {
  // Lazy-loads the model on first run().
  HumanFaceDetect *detect = new HumanFaceDetect();
  detect->set_score_thr(FACE_SCORE_THR, 0);
  detect->set_score_thr(FACE_SCORE_THR, 1);
  int64_t last_seen = 0;
  int64_t last_log = 0;
  int64_t detect_ms_total = 0;
  int detect_count = 0;
  uint32_t frame_luma = 0, last_hash = 0;
  int frozen_frames = 0;
  bool seen_someone = false, announced_gone = false;

  while (1) {
    // Detection competes with audio for CPU and flash/PSRAM bandwidth (19
    // underruns in one reply when running flat out, and garbled first words
    // when a detection overlapped the start of a reply). Only run while idle
    // or listening, so it's already quiet when a reply starts; the eyes hold
    // their last position.
    face_state_t state = pipecat_face_state;
    if (pipecat_audio_is_playing() ||
        (state != FACE_IDLE && state != FACE_LISTENING)) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    int64_t started = now_ms();
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
      ESP_LOGW(LOG_TAG, "Face track: camera capture failed");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // Diagnostics: average brightness and a cheap fingerprint, to tell a
    // dark or frozen image apart from a detection problem.
    {
      const uint8_t *p = (const uint8_t *)fb->buf;
      uint32_t luma = 0, hash = 0, n = 0;
      for (size_t i = 0; i + 1 < fb->len; i += 2 * 97) {
        uint16_t px = (p[i] << 8) | p[i + 1];  // RGB565 big-endian
        luma += ((px >> 11) & 0x1f) * 8 + ((px >> 5) & 0x3f) * 4 + (px & 0x1f) * 8;
        hash = hash * 31 + px;
        n++;
      }
      frame_luma = n ? luma / n / 3 : 0;
      if (hash == last_hash) {
        frozen_frames++;
      }
      last_hash = hash;
    }

    dl::image::img_t img = {
        .data = fb->buf,
        .width = (uint16_t)fb->width,
        .height = (uint16_t)fb->height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };
    int64_t t0 = now_ms();
    auto &results = detect->run(img);
    detect_ms_total += now_ms() - t0;
    detect_count++;

    const dl::detect::result_t *best = NULL;
    for (const auto &r : results) {
      if (best == NULL || r.score > best->score) {
        best = &r;
      }
    }
    if (best != NULL) {
      int cx = (best->box[0] + best->box[2]) / 2;
      int cy = (best->box[1] + best->box[3]) / 2;
      pipecat_face_target_x = (cx - (int)fb->width / 2) * 100 / ((int)fb->width / 2);
      pipecat_face_target_y = (cy - (int)fb->height / 2) * 100 / ((int)fb->height / 2);
      if (!pipecat_face_present) {
        ESP_LOGI(LOG_TAG, "Face track: face found (score %.2f)", best->score);
      }
      pipecat_face_present = true;
      last_seen = now_ms();
    } else if (pipecat_face_present && now_ms() - last_seen > FACE_LOST_MS) {
      pipecat_face_present = false;
      ESP_LOGI(LOG_TAG, "Face track: face lost");
    }
    if (FACE_TRACK_PREVIEW) {
      // Camera frames are big-endian RGB565, which M5GFX treats as native
      // when swapBytes is false (checked side by side on the device).
      M5.Display.startWrite();
      M5.Display.setSwapBytes(false);
      M5.Display.pushImage(0, 0, fb->width, fb->height, (const uint16_t *)fb->buf);
      for (const auto &r : results) {
        M5.Display.drawRect(r.box[0], r.box[1], r.box[2] - r.box[0], r.box[3] - r.box[1], TFT_GREEN);
      }
      M5.Display.endWrite();
    }
    esp_camera_fb_return(fb);

    // Presence lines ("where have you gone?" / "there you are!"), off: the
    // eyes just follow, with no actions when nobody's found.
    if (PRESENCE_LINES && pipecat_face_present) {
      seen_someone = true;
      if (announced_gone) {
        announced_gone = false;
        pipecat_rtvi_send_client_message("user_returned");
      }
    } else if (PRESENCE_LINES && seen_someone && !announced_gone &&
               pipecat_face_state == FACE_IDLE) {
      announced_gone = true;
      pipecat_rtvi_send_client_message("user_left");
    }

    if (now_ms() - last_log > FACE_TRACK_LOG_MS) {
      uint32_t sent;
      int stage;
      pipecat_audio_stats(&sent, &stage);
      ESP_LOGI(LOG_TAG, "Audio out: %lu frames sent, stage %d",
               (unsigned long)sent, stage);
      ESP_LOGI(LOG_TAG,
               "Face track: detect avg %lldms, luma %lu, frozen %d/%d, "
               "present %d, x %d, y %d, "
               "internal free %uK (largest %uK, min ever %uK)",
               detect_count ? detect_ms_total / detect_count : 0,
               (unsigned long)frame_luma, frozen_frames, detect_count,
               (int)pipecat_face_present, pipecat_face_target_x,
               pipecat_face_target_y,
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
               heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
      detect_ms_total = 0;
      detect_count = 0;
      frozen_frames = 0;
      last_log = now_ms();
    }

    int64_t elapsed = now_ms() - started;
    int delay = FACE_TRACK_INTERVAL_MS - (int)elapsed;
    vTaskDelay(pdMS_TO_TICKS(delay > 10 ? delay : 10));
  }
}

void pipecat_face_tracking_init() {
#if FACE_TRACK_OFF
  ESP_LOGW(LOG_TAG, "Face track: disabled at build time");
  return;
#endif
  // CoreS3 camera pins (from espressif/m5stack_core_s3 BSP 3.0.2). No XCLK
  // (the module has its own clock); reset is on the AW9523 expander, which
  // M5GFX already releases. SCCB pins NC + port 1 = reuse the internal I2C
  // bus M5Unified already set up.
  camera_config_t config = {};
  config.pin_pwdn = GPIO_NUM_NC;
  config.pin_reset = GPIO_NUM_NC;
  config.pin_xclk = GPIO_NUM_NC;
  config.pin_sccb_sda = GPIO_NUM_NC;
  config.pin_sccb_scl = GPIO_NUM_NC;
  config.pin_d7 = GPIO_NUM_47;
  config.pin_d6 = GPIO_NUM_48;
  config.pin_d5 = GPIO_NUM_16;
  config.pin_d4 = GPIO_NUM_15;
  config.pin_d3 = GPIO_NUM_42;
  config.pin_d2 = GPIO_NUM_41;
  config.pin_d1 = GPIO_NUM_40;
  config.pin_d0 = GPIO_NUM_39;
  config.pin_vsync = GPIO_NUM_46;
  config.pin_href = GPIO_NUM_38;
  config.pin_pclk = GPIO_NUM_45;
  config.xclk_freq_hz = 10000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  // One buffer, filled only when free: the camera captures a frame each time
  // we hand the last one back (~5 fps) instead of streaming ~25 fps.
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.sccb_i2c_port = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Face track: camera init failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(LOG_TAG, "Face track: camera ready, internal free %uK",
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

  xTaskCreatePinnedToCore(face_track_task, "face_track", 12288, NULL, 1, NULL, 1);
}
