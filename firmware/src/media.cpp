#include <opus.h>

#include <atomic>
#include <cstring>
#include <cstdio>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "main.h"

#define SAMPLE_RATE (16000)

#define OPUS_BUFFER_SIZE 1276  // 1276 bytes is recommended by opus_encode
#define PCM_BUFFER_SIZE 640

#define OPUS_ENCODER_BITRATE 30000
#define OPUS_ENCODER_COMPLEXITY 0

// Playback ring. M5.Speaker.playRaw() does not copy samples: it reads them
// later from its own task, so every frame needs its own buffer that stays
// untouched until played. A feeder task keeps the speaker's two slots on a
// fixed channel loaded; the WebRTC loop only decodes into the ring.
#define DECODE_MAX_SAMPLES 960    // Opus packets up to 60ms at 16kHz
#define OUTPUT_FRAME_SAMPLES 320  // ring slots are 20ms
// 0.96s. Kept in PSRAM: in internal RAM it left only ~24K free for Wi-Fi.
#define OUTPUT_RING_FRAMES 48
#define OUTPUT_SPEAKER_HELD 3     // frames M5Unified may still be reading
#define OUTPUT_PREROLL_FRAMES 6   // 120ms buffered before playback starts
#define OUTPUT_CHANNEL 0
#define OUTPUT_GAIN_Q8 256        // 256 = 1.0x; the stock 2x gain clipped
#define SPEAKER_VOLUME 255

typedef struct {
  int16_t samples[OUTPUT_FRAME_SAMPLES];
  size_t len;
} output_frame_t;

static output_frame_t *output_ring = NULL;
static int16_t *decode_scratch = NULL;
static std::atomic<uint32_t> ring_write = 0;
static std::atomic<uint32_t> ring_read = 0;
static std::atomic<bool> playback_started = false;
static std::atomic<uint32_t> output_drops = 0;
static std::atomic<uint32_t> output_underruns = 0;
// Playback timing: real rate for the first second vs overall, and the longest
// gap between feeds, to tell "plays too fast / starved" from "arrives broken".
static int64_t play_first_ms = 0, play_last_feed_ms = 0, play_max_gap_ms = 0;
static uint32_t play_frames = 0, play_frames_first_sec = 0;

#define AUDIO_WATCHDOG_MS 5000
static std::atomic<int64_t> last_audio_ms = 0;

// After a tap-to-interrupt, audio already in flight from the server is
// dropped until it goes quiet, so the cut-off reply doesn't start again.
#define INTERRUPT_QUIET_FRAMES 10
#define INTERRUPT_MAX_MS 3000
static std::atomic<bool> discarding = false;
static int64_t discard_started_ms = 0;
static int discard_quiet_frames = 0;

std::atomic<bool> is_playing = false;
unsigned int silence_count = 0;

void set_is_playing(int16_t *in_buf, size_t in_samples) {
  bool any_set = false;
  for (size_t i = 0; i < in_samples; i++) {
    if (in_buf[i] != -1 && in_buf[i] != 0 && in_buf[i] != 1) {
      any_set = true;
    }
  }

  if (any_set) {
    silence_count = 0;
  } else {
    silence_count++;
  }

  // Only flip the flag here. The mic/speaker switch happens in the audio
  // publisher task (sync_audio_mode), the only task that touches M5.Mic:
  // ending the mic while that task is inside M5.Mic.record() hangs it.
  if (silence_count >= 20 && is_playing) {
    is_playing = false;
    int64_t span = play_last_feed_ms - play_first_ms;
    ESP_LOGI(LOG_TAG,
             "Playback done: underruns %lu drops %lu, %lu frames in %lldms "
             "(%lu in first second, expect 50), max feed gap %lldms",
             (unsigned long)output_underruns.load(),
             (unsigned long)output_drops.load(), (unsigned long)play_frames,
             span, (unsigned long)play_frames_first_sec, play_max_gap_ms);
  } else if (any_set && !is_playing) {
    // The feeder is idle while not playing, so discard leftovers by moving
    // the reader up to the frame being decoded right now.
    ring_read = ring_write.load();
    playback_started = false;
    output_underruns = 0;
    output_drops = 0;
    play_first_ms = play_last_feed_ms = play_max_gap_ms = 0;
    play_frames = play_frames_first_sec = 0;
    is_playing = true;
  }
}

static bool speaker_mode = false;
// Set only once the speaker has really been started (and the mic stopped) by
// the publisher task. The feeder must wait for it: M5.Speaker.isEnabled()
// only means "pins configured", and playRaw() would otherwise start the
// speaker itself while the mic still owns the shared I2S bus, which played
// the start of each reply at the wrong clock (sped up and garbled).
static std::atomic<bool> speaker_ready = false;

// M5.Mic.record() is asynchronous with two slots: when it returns, the buffer
// queued two calls earlier is complete and the previous one may still be
// filling. The stock code reused one buffer (encoding half-written audio), so
// rotate three and always encode the finished one.
#define MIC_BUFFERS 3
static int16_t *mic_buffers[MIC_BUFFERS] = {};
static int mic_idx = 0;

bool pipecat_audio_is_playing() { return is_playing; }

// Heartbeat for the outgoing (mic) path, logged by the face tracker.
static std::atomic<uint32_t> frames_sent = 0;
static std::atomic<int> send_stage = 0;
void pipecat_audio_stats(uint32_t *sent, int *stage) {
  *sent = frames_sent;
  *stage = send_stage;
}

static void sync_audio_mode() {
  bool want_speaker = is_playing;
  if (want_speaker == speaker_mode) {
    return;
  }
  if (want_speaker) {
    M5.Mic.end();
    M5.Speaker.begin();
    speaker_ready = true;
  } else {
    speaker_ready = false;
    M5.Speaker.end();
    for (int i = 0; i < MIC_BUFFERS; i++) {
      memset(mic_buffers[i], 0, PCM_BUFFER_SIZE);
    }
    M5.Mic.begin();
  }
  speaker_mode = want_speaker;
}

static void apply_gain(int16_t *samples, size_t num_samples) {
  if (OUTPUT_GAIN_Q8 == 256) {
    return;
  }
  for (size_t i = 0; i < num_samples; i++) {
    int32_t amplified = ((int32_t)samples[i] * OUTPUT_GAIN_Q8) >> 8;
    if (amplified > INT16_MAX) {
      amplified = INT16_MAX;
    } else if (amplified < INT16_MIN) {
      amplified = INT16_MIN;
    }
    samples[i] = (int16_t)amplified;
  }
}

static void speaker_feed_task(void *) {
  while (1) {
    // The bot streams audio (or silence) continuously while connected. If it
    // stops, the session is dead and libpeer won't notice (keepalive is off),
    // so reboot and reconnect rather than sit there looking idle.
    int64_t last = last_audio_ms;
    if (last != 0 &&
        esp_timer_get_time() / 1000 - last > AUDIO_WATCHDOG_MS) {
      ESP_LOGE(LOG_TAG, "No audio from server for %dms, restarting",
               AUDIO_WATCHDOG_MS);
      pipecat_restart("no audio from server");
    }

    if (is_playing && speaker_ready) {
      uint32_t r = ring_read;
      uint32_t w = ring_write;

      if (!playback_started && w - r >= OUTPUT_PREROLL_FRAMES) {
        playback_started = true;
      }

      if (playback_started) {
        while (r != w && M5.Speaker.isPlaying(OUTPUT_CHANNEL) < 2) {
          output_frame_t *frame = &output_ring[r % OUTPUT_RING_FRAMES];
          if (!M5.Speaker.playRaw(frame->samples, frame->len, SAMPLE_RATE,
                                  false, 1, OUTPUT_CHANNEL)) {
            break;
          }
          ring_read = ++r;
          int64_t t = esp_timer_get_time() / 1000;
          if (play_frames == 0) {
            play_first_ms = t;
          } else if (t - play_last_feed_ms > play_max_gap_ms) {
            play_max_gap_ms = t - play_last_feed_ms;
          }
          if (t - play_first_ms < 1000) {
            play_frames_first_sec++;
          }
          play_last_feed_ms = t;
          play_frames++;
        }

        // Speaker ran dry: rebuild the preroll rather than play in bursts.
        if (r == w && M5.Speaker.isPlaying(OUTPUT_CHANNEL) == 0) {
          output_underruns++;
          playback_started = false;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void pipecat_init_audio_capture() {
  auto spk_cfg = M5.Speaker.config();
  spk_cfg.dma_buf_len = 512;
  // Pinned to core 0, away from the camera driver's task (priority 23) and
  // face detection on core 1; unpinned it could land beside them and fall
  // behind while mixing, glitching playback whenever tracking was active.
  spk_cfg.task_priority = 5;
  spk_cfg.task_pinned_core = 0;
  M5.Speaker.config(spk_cfg);
  M5.Speaker.setVolume(SPEAKER_VOLUME);

  // Keep the mic task off core 1, where the camera task (priority 23) and
  // face detection run; starved, it stalls M5.Mic.record() and the bot stops
  // hearing us.
  auto mic_cfg = M5.Mic.config();
  mic_cfg.task_pinned_core = 0;
  mic_cfg.task_priority = 6;
  M5.Mic.config(mic_cfg);
}

OpusDecoder *opus_decoder = NULL;

void pipecat_init_audio_decoder() {
  int decoder_error = 0;
  opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &decoder_error);
  if (decoder_error != OPUS_OK) {
    printf("Failed to create OPUS decoder");
    return;
  }

  output_ring = (output_frame_t *)heap_caps_malloc(
      sizeof(output_frame_t) * OUTPUT_RING_FRAMES, MALLOC_CAP_SPIRAM);
  decode_scratch = (int16_t *)heap_caps_malloc(DECODE_MAX_SAMPLES * sizeof(int16_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  xTaskCreatePinnedToCore(speaker_feed_task, "speaker_feed", 3072, NULL, 4,
                          NULL, 1);
}

void pipecat_audio_decode(uint8_t *data, size_t size) {
  last_audio_ms = esp_timer_get_time() / 1000;

  // Always decode so the Opus decoder state stays continuous, even when the
  // audio is then discarded or dropped.
  int decoded_size = opus_decode(opus_decoder, data, size, decode_scratch,
                                 DECODE_MAX_SAMPLES, 0);
  if (decoded_size <= 0) {
    return;
  }

  if (discarding) {
    bool quiet = true;
    for (int i = 0; i < decoded_size; i++) {
      if (decode_scratch[i] > 1 || decode_scratch[i] < -1) {
        quiet = false;
        break;
      }
    }
    discard_quiet_frames = quiet ? discard_quiet_frames + 1 : 0;
    if (discard_quiet_frames >= INTERRUPT_QUIET_FRAMES ||
        last_audio_ms - discard_started_ms > INTERRUPT_MAX_MS) {
      discarding = false;
      silence_count = 0;
      ESP_LOGI(LOG_TAG, "Interrupt: server audio quiet, playback re-armed");
    }
    return;
  }

  set_is_playing(decode_scratch, decoded_size);
  if (!is_playing) {
    return;
  }

  apply_gain(decode_scratch, decoded_size);
  for (int off = 0; off < decoded_size; off += OUTPUT_FRAME_SAMPLES) {
    uint32_t w = ring_write;
    if (w - ring_read >= OUTPUT_RING_FRAMES - OUTPUT_SPEAKER_HELD) {
      output_drops++;
      break;
    }
    output_frame_t *frame = &output_ring[w % OUTPUT_RING_FRAMES];
    int n = decoded_size - off;
    if (n > OUTPUT_FRAME_SAMPLES) {
      n = OUTPUT_FRAME_SAMPLES;
    }
    memcpy(frame->samples, decode_scratch + off, n * sizeof(int16_t));
    frame->len = n;
    ring_write = w + 1;
  }
}

OpusEncoder *opus_encoder = NULL;
uint8_t *encoder_output_buffer = NULL;
int16_t *read_buffer = NULL;

void pipecat_init_audio_encoder() {
  int encoder_error;
  opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP,
                                     &encoder_error);
  if (encoder_error != OPUS_OK) {
    printf("Failed to create OPUS encoder");
    return;
  }

  if (opus_encoder_init(opus_encoder, SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP) !=
      OPUS_OK) {
    printf("Failed to initialize OPUS encoder");
    return;
  }

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  read_buffer = (int16_t *)heap_caps_malloc(PCM_BUFFER_SIZE, MALLOC_CAP_DEFAULT);
  encoder_output_buffer = (uint8_t *)malloc(OPUS_BUFFER_SIZE);

  for (int i = 0; i < MIC_BUFFERS; i++) {
    mic_buffers[i] = (int16_t *)heap_caps_calloc(1, PCM_BUFFER_SIZE, MALLOC_CAP_INTERNAL);
  }

  // Runs on the audio publisher task, which owns mic/speaker switching.
  M5.Speaker.end();
  M5.Mic.begin();
  speaker_mode = false;
}

static void interrupt_playback() {
  discard_quiet_frames = 0;
  discard_started_ms = esp_timer_get_time() / 1000;
  discarding = true;
  is_playing = false;
  sync_audio_mode();  // speaker off, mic on, right now
  pipecat_face_state = FACE_LISTENING;
  pipecat_rtvi_send_client_message("interrupt");
  ESP_LOGI(LOG_TAG, "Interrupt: tapped while speaking");
}

// Touch shares the internal I2C bus with the mic/speaker codecs, so it's
// polled here, in the only task that reconfigures them.
static void poll_touch() {
  M5.update();
  if (M5.Touch.getCount() == 0) {
    return;
  }
  auto detail = M5.Touch.getDetail(0);
  if (detail.wasPressed() && speaker_mode) {
    interrupt_playback();
  }
}

void pipecat_send_audio(PeerConnection *peer_connection) {
  send_stage = 1;
  poll_touch();
  send_stage = 2;
  sync_audio_mode();
  send_stage = 3;
  const int16_t *pcm = read_buffer;
  if (speaker_mode) {
    memset(read_buffer, 0, PCM_BUFFER_SIZE);
    vTaskDelay(pdMS_TO_TICKS(20));
  } else {
    // Blocks until a slot frees up, which paces this loop at real time.
    if (!M5.Mic.record(mic_buffers[mic_idx], PCM_BUFFER_SIZE / sizeof(uint16_t),
                       SAMPLE_RATE)) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    mic_idx = (mic_idx + 1) % MIC_BUFFERS;
    pcm = mic_buffers[mic_idx];  // queued two calls ago: complete
  }
  send_stage = 4;

  auto encoded_size = opus_encode(opus_encoder, (const opus_int16 *)pcm,
                                  PCM_BUFFER_SIZE / sizeof(uint16_t),
                                  encoder_output_buffer, OPUS_BUFFER_SIZE);
  send_stage = 5;
  peer_connection_send_audio(peer_connection, encoder_output_buffer,
                             encoded_size);
  send_stage = 6;
  frames_sent++;
}
