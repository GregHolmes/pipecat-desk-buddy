#include <cJSON.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "main.h"

#define MAX_TYPE_LEN 32
#define MAX_ID_LEN 64

static int rtvi_id = 0;
static QueueHandle_t rtvi_queue;
static PeerConnection *peer_connection = NULL;
static rtvi_callbacks_t *rtvi_callbacks = NULL;

typedef struct {
  cJSON *msg;
} rtvi_msg_t;

// Simple hashing function so we can fake pattern matching and switch on strings
// as a constexpr so it gets evaluated in compile time for static strings
static constexpr unsigned int hash(const char *s, int off = 0) {
  return !s[off] ? 5381 : (hash(s, off + 1) * 33) ^ s[off];
}

static rtvi_msg_t *create_rtvi_message(const char *type) {
  cJSON *j_msg = cJSON_CreateObject();

  if (j_msg == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }
  if (cJSON_AddStringToObject(j_msg, "label", "rtvi-ai") == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }
  if (cJSON_AddStringToObject(j_msg, "type", type) == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }

  char id[MAX_ID_LEN];
  sprintf(id, "%d", rtvi_id++);
  if (cJSON_AddStringToObject(j_msg, "id", id) == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }

  rtvi_msg_t *msg = (rtvi_msg_t *)malloc(sizeof(rtvi_msg_t));
  msg->msg = j_msg;

  return msg;
}

static void destroy_rtvi_message(rtvi_msg_t *msg) {
  cJSON_Delete(msg->msg);
  free(msg);
}

static char *rtvi_message_to_string(rtvi_msg_t *msg) {
  if (msg == NULL || msg->msg == NULL) {
    return NULL;
  }

  char *msg_str = cJSON_Print(msg->msg);

  return msg_str;
}

static void rtvi_handle_message(const rtvi_msg_t *msg) {
  cJSON *j_type = cJSON_GetObjectItem(msg->msg, "type");
  if (!cJSON_IsString(j_type)) {
    ESP_LOGE(LOG_TAG, "Unable to find `type` field in RTVI message");
    return;
  }

  switch (hash(j_type->valuestring)) {
    case hash("bot-started-speaking"):
      rtvi_callbacks->on_bot_started_speaking();
      break;
    case hash("bot-stopped-speaking"):
      rtvi_callbacks->on_bot_stopped_speaking();
      break;
    case hash("bot-tts-text"): {
      cJSON *j_data = cJSON_GetObjectItem(msg->msg, "data");
      cJSON *j_text = cJSON_GetObjectItem(j_data, "text");
      if (cJSON_IsString(j_text)) {
        rtvi_callbacks->on_bot_tts_text(j_text->valuestring);
      }
      break;
    }
    case hash("user-started-speaking"):
      rtvi_callbacks->on_user_started_speaking();
      break;
    case hash("user-stopped-speaking"):
      rtvi_callbacks->on_user_stopped_speaking();
      break;
    case hash("llm-function-call-started"): {
      cJSON *j_data = cJSON_GetObjectItem(msg->msg, "data");
      cJSON *j_name = cJSON_GetObjectItem(j_data, "function_name");
      rtvi_callbacks->on_function_call_started(
          cJSON_IsString(j_name) ? j_name->valuestring : "");
      break;
    }
    case hash("llm-function-call-stopped"):
      rtvi_callbacks->on_function_call_stopped();
      break;
    case hash("server-message"): {
      cJSON *j_data = cJSON_GetObjectItem(msg->msg, "data");
      cJSON *j_event = cJSON_GetObjectItem(j_data, "event");
      if (cJSON_IsString(j_event)) {
        rtvi_callbacks->on_server_message_event(j_event->valuestring);
      }
      break;
    }
    default:
      break;
  }
}

static void rtvi_task(void *pvParameter) {
  rtvi_msg_t msg;

  while (1) {
    if (xQueueReceive(rtvi_queue, &msg, portMAX_DELAY)) {
      rtvi_handle_message(&msg);
      cJSON_Delete(msg.msg);
    }
  }
}

void pipecat_init_rtvi(PeerConnection *connection,
                       rtvi_callbacks_t *callbacks) {
  peer_connection = connection;
  rtvi_callbacks = callbacks;

  rtvi_queue = xQueueCreate(10, sizeof(rtvi_msg_t));
  xTaskCreatePinnedToCore(rtvi_task, "RTVI Task", 4096, NULL, 2, NULL, 1);
}

void pipecat_rtvi_send_client_ready() {
  if (peer_connection == NULL) {
    return;
  }
  rtvi_msg_t *msg = create_rtvi_message("client-ready");
  if (msg == NULL) {
    return;
  }
  // A protocol version avoids the server's "Client version unknown" error.
  // 1.x is the legacy major the server still accepts.
  cJSON *j_data = cJSON_AddObjectToObject(msg->msg, "data");
  cJSON_AddStringToObject(j_data, "version", "1.0.0");
  cJSON *j_about = cJSON_AddObjectToObject(j_data, "about");
  cJSON_AddStringToObject(j_about, "library", "pipecat-esp32");
  cJSON_AddStringToObject(j_about, "platform", "M5Stack CoreS3");

  char *msg_str = cJSON_PrintUnformatted(msg->msg);
  ESP_LOGI(LOG_TAG, "Sent client-ready");

  peer_connection_datachannel_send(peer_connection, msg_str, strlen(msg_str));

  cJSON_free(msg_str);

  destroy_rtvi_message(msg);
}

// RTVI client-message: {"type": "client-message", "data": {"t": type, "d": {}}}
// libpeer isn't thread-safe: sending on the data channel (DTLS) from another
// task while the main loop reads from it corrupted the connection (the device
// went deaf right after sending "user_left"). Other tasks only enqueue; the
// main loop sends, via pipecat_rtvi_flush_outgoing().
static QueueHandle_t outgoing_queue = NULL;

void pipecat_rtvi_send_client_message(const char *type) {
  if (outgoing_queue == NULL) {
    outgoing_queue = xQueueCreate(8, sizeof(const char *));
  }
  // `type` must be a string literal (static lifetime).
  xQueueSend(outgoing_queue, &type, 0);
}

static void send_client_message_now(const char *type) {
  if (peer_connection == NULL) {
    return;  // not connected yet
  }
  rtvi_msg_t *msg = create_rtvi_message("client-message");
  if (msg == NULL) {
    return;
  }
  cJSON *j_data = cJSON_AddObjectToObject(msg->msg, "data");
  cJSON_AddStringToObject(j_data, "t", type);
  cJSON_AddObjectToObject(j_data, "d");

  char *msg_str = cJSON_PrintUnformatted(msg->msg);
  peer_connection_datachannel_send(peer_connection, msg_str, strlen(msg_str));
  ESP_LOGI(LOG_TAG, "Sent client-message: %s", type);

  cJSON_free(msg_str);
  destroy_rtvi_message(msg);
}

void pipecat_rtvi_flush_outgoing() {
  const char *type;
  while (outgoing_queue != NULL && xQueueReceive(outgoing_queue, &type, 0)) {
    send_client_message_now(type);
  }
}

void pipecat_rtvi_handle_message(const char *msg, size_t len) {
  // Data channel payloads are not NUL-terminated.
  cJSON *j_msg = cJSON_ParseWithLength(msg, len);
  if (j_msg == NULL) {
    ESP_LOGE(LOG_TAG, "Error parsing RTVI message");
    return;
  }

  rtvi_msg_t rtvi_msg = {.msg = j_msg};

  // Called from the WebRTC loop: never block it (that stalled incoming audio
  // when a burst of messages filled the queue). Drop instead.
  if (xQueueSend(rtvi_queue, &rtvi_msg, 0) != pdTRUE) {
    ESP_LOGW(LOG_TAG, "RTVI queue full, dropping message");
    cJSON_Delete(j_msg);
  }
}
