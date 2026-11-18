#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>

#ifndef LINUX_BUILD
#include "nvs_flash.h"

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setBrightness(70);
  pipecat_face_init();
  pipecat_face_tracking_init();

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  pipecat_init_audio_capture();
  pipecat_init_audio_decoder();
  pipecat_init_wifi();
  pipecat_init_webrtc();

  // libpeer reads one UDP packet per loop call, and the bot sends 50 audio
  // packets a second plus data channel bursts. At the stock 15ms interval
  // the 6-slot lwIP mailbox overflowed, dropping audio and RTVI messages.
  // When idle each call blocks ~1ms in select(), so this doesn't spin.
  while (1) {
    pipecat_webrtc_loop();
    vTaskDelay(pdMS_TO_TICKS(WEBRTC_LOOP_INTERVAL_MS));
  }
}
#else
int main(void) {
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  pipecat_webrtc();

  while (1) {
    pipecat_webrtc_loop();
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif
