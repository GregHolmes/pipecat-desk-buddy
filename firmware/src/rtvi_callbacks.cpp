#include <esp_log.h>
#include <string.h>

#include "main.h"

volatile face_state_t pipecat_face_state = FACE_CONNECTING;
const char *volatile pipecat_face_caption = "";

// Tool calls in flight. Claude may say "let me check" first, so speech can
// stop while a lookup is still running.
static volatile int function_calls_active = 0;

const char *face_state_name(face_state_t state) {
  switch (state) {
    case FACE_CONNECTING:
      return "connecting";
    case FACE_IDLE:
      return "idle";
    case FACE_LISTENING:
      return "listening";
    case FACE_THINKING:
      return "thinking";
    case FACE_HESITATING:
      return "hesitating";
    case FACE_SPEAKING:
      return "speaking";
    case FACE_LOOKING:
      return "looking";
  }
  return "unknown";
}

static void set_face_state(face_state_t state, const char *reason) {
  pipecat_face_state = state;
  ESP_LOGI(LOG_TAG, "Face: %s (%s)", face_state_name(state), reason);
}

static void on_bot_started_speaking() {
  set_face_state(FACE_SPEAKING, "bot-started-speaking");
}

static void on_bot_stopped_speaking() {
  if (function_calls_active > 0) {
    set_face_state(FACE_LOOKING, "bot-stopped-speaking, lookup running");
  } else {
    set_face_state(FACE_IDLE, "bot-stopped-speaking");
  }
}

static void on_bot_tts_text(const char *text) {}

static void on_user_started_speaking() {
  // Starting again while we're still "thinking" (the bot held the turn as
  // incomplete and hasn't replied) is the user carrying on: "oh, go on".
  if (pipecat_face_state == FACE_THINKING) {
    set_face_state(FACE_HESITATING, "user-started-speaking while thinking");
  } else {
    set_face_state(FACE_LISTENING, "user-started-speaking");
  }
}

static void on_user_stopped_speaking() {
  set_face_state(FACE_THINKING, "user-stopped-speaking");
}

static void on_server_message_event(const char *event) {
  if (strcmp(event, "eager_eot") == 0) {
    set_face_state(FACE_THINKING, event);
  } else if (strcmp(event, "turn_resumed") == 0) {
    set_face_state(FACE_HESITATING, event);
  }
}

static const char *caption_for(const char *function_name) {
  if (strcmp(function_name, "get_weather") == 0) {
    return "checking the weather";
  }
  if (strcmp(function_name, "get_current_time") == 0) {
    return "checking the clock";
  }
  if (strcmp(function_name, "search_web") == 0) {
    return "searching the web";
  }
  return "looking it up";
}

static void on_function_call_started(const char *function_name) {
  function_calls_active = function_calls_active + 1;
  pipecat_face_caption = caption_for(function_name);
  set_face_state(FACE_LOOKING, function_name);
}

static void on_function_call_stopped() {
  if (function_calls_active > 0) {
    function_calls_active = function_calls_active - 1;
  }
  if (function_calls_active == 0 && pipecat_face_state == FACE_LOOKING) {
    set_face_state(FACE_THINKING, "function call done");
  }
}

rtvi_callbacks_t pipecat_rtvi_callbacks = {
    .on_bot_started_speaking = on_bot_started_speaking,
    .on_bot_stopped_speaking = on_bot_stopped_speaking,
    .on_bot_tts_text = on_bot_tts_text,
    .on_user_started_speaking = on_user_started_speaking,
    .on_user_stopped_speaking = on_user_stopped_speaking,
    .on_server_message_event = on_server_message_event,
    .on_function_call_started = on_function_call_started,
    .on_function_call_stopped = on_function_call_stopped,
};
