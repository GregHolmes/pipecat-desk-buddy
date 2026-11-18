#include <assert.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"

static bool g_wifi_connected = false;

static void pipecat_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  static int s_retry_num = 0;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < 5) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(LOG_TAG, "retry to connect to the AP");
    }
    ESP_LOGI(LOG_TAG, "connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(LOG_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    g_wifi_connected = true;
  }
}

void pipecat_init_wifi() {
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &pipecat_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &pipecat_event_handler, NULL));

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  assert(sta_netif);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Stay in the default modem sleep while connecting. If the AP still thinks
  // we're dozing (stale session after a reboot), it buffers unicast for us,
  // and only a power-saving station fetches it: with PS off, the offer's TCP
  // replies never arrived for 30 to 90s. Power save goes off once the call is
  // up (pipecat_wifi_low_latency), where it would add jitter to audio.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

  ESP_LOGI(LOG_TAG, "Connecting to WiFi");
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, (char *)WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, (char *)WIFI_PASSWORD,
          sizeof(wifi_config.sta.password));

  ESP_ERROR_CHECK(esp_wifi_set_config(
      static_cast<wifi_interface_t>(ESP_IF_WIFI_STA), &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_connect());

  // block until we get an IP address
  while (!g_wifi_connected) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  // The first offer right after getting an IP consistently timed out (the
  // device wasn't reachable yet), costing a 10s timeout plus a reboot.
  vTaskDelay(pdMS_TO_TICKS(2000));
}

// Reboot, but tell the AP we're leaving first. A plain esp_restart() leaves
// the old association alive on the router, and on the next boot it accepts
// us but forwards no data until that stale session times out (30s+).
void pipecat_restart(const char *reason) {
  ESP_LOGW(LOG_TAG, "Restarting: %s", reason);
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}

// Once the call is up: no modem sleep, so inbound audio isn't batched around
// beacons.
void pipecat_wifi_low_latency() {
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_LOGI(LOG_TAG, "Wi-Fi power save off for the call");
}
