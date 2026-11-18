#include <peer.h>

#ifndef LINUX_BUILD
#include <M5Unified.h>
#endif

#define LOG_TAG "pipecat"
#define MAX_HTTP_OUTPUT_BUFFER 4096
#define HTTP_TIMEOUT_MS 10000
#define HTTP_OFFER_ATTEMPTS 8
#define HTTP_OFFER_ATTEMPT_TIMEOUT_MS 3000
#define TICK_INTERVAL 15
#define WEBRTC_LOOP_INTERVAL_MS 1

// Wifi
extern void pipecat_init_wifi();
extern void pipecat_restart(const char *reason);
extern void pipecat_wifi_low_latency();

// WebRTC / Media
extern void pipecat_init_audio_capture();
extern void pipecat_init_audio_decoder();
extern void pipecat_init_audio_encoder();
extern void pipecat_send_audio(PeerConnection *peer_connection);
extern void pipecat_audio_decode(uint8_t *data, size_t size);
extern bool pipecat_audio_is_playing();
extern void pipecat_audio_arm_watchdog();
// Mic path heartbeat: frames sent; stage 1 touch, 2 mode, 3 record, 4 encode, 5 send, 6 done.
extern void pipecat_audio_stats(uint32_t *sent, int *stage);

// WebRTC / Signalling
extern void pipecat_init_webrtc();
extern void pipecat_webrtc_loop();
extern void pipecat_http_request(char *offer, char *answer);

// RTVI
typedef struct {
  void (*on_bot_started_speaking)();
  void (*on_bot_stopped_speaking)();
  void (*on_bot_tts_text)(const char *text);
  void (*on_user_started_speaking)();
  void (*on_user_stopped_speaking)();
  void (*on_server_message_event)(const char *event);
  void (*on_function_call_started)(const char *function_name);
  void (*on_function_call_stopped)();
} rtvi_callbacks_t;

extern rtvi_callbacks_t pipecat_rtvi_callbacks;

extern void pipecat_init_rtvi(PeerConnection *peer_connection, rtvi_callbacks_t *callbacks);
extern void pipecat_rtvi_send_client_ready();
// Safe from any task: queued, then sent from the main loop.
extern void pipecat_rtvi_send_client_message(const char *type);
extern void pipecat_rtvi_flush_outgoing();
extern void pipecat_rtvi_handle_message(const char* msg, size_t len);

// Face
typedef enum {
  FACE_CONNECTING,
  FACE_IDLE,
  FACE_LISTENING,
  FACE_THINKING,
  FACE_HESITATING,
  FACE_SPEAKING,
  FACE_LOOKING,
} face_state_t;

// Written by RTVI callbacks, read by the display task. Callbacks must only
// set this and return; never draw from them.
extern volatile face_state_t pipecat_face_state;
// Caption shown under the face while looking something up (static strings).
extern const char *volatile pipecat_face_caption;
extern const char *face_state_name(face_state_t state);
extern void pipecat_face_init();

// Face tracking (camera + on-device face detection)
extern void pipecat_face_tracking_init();
extern volatile int pipecat_face_target_x;  // -100 .. 100
extern volatile int pipecat_face_target_y;  // -100 .. 100
extern volatile bool pipecat_face_present;

// Screen
extern void pipecat_init_screen();
extern void pipecat_screen_system_log(const char *text);
extern void pipecat_screen_new_log();
extern void pipecat_screen_log(const char *text);
