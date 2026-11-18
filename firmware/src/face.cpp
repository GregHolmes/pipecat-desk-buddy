#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <stdlib.h>

#include "main.h"

// The face runs on its own low-priority task so drawing never blocks the
// WebRTC loop or the speaker feeder. Eyes and mouth are separate sprites and
// each is only pushed when what it shows has changed.

#define FACE_TICK_MS 40
#define HESITATE_HOLD_MS 1400
#define HESITATE_MIN_MS 1300
#define HESITATE_BOUNCE_MS 160
#define MIN_DWELL_MS 400
#define BLINK_MS 120
#define TALK_STEP_MS 90
#define DOTS_STEP_MS 250
#define SCAN_STEP_MS 90

// Eyes follow the tracked face. The camera sees the scene as the device does,
// so a face on the image's right is on the device's right, which is the
// screen's left as the user sees it: hence the mirror.
// Gaze is exaggerated on purpose: it has to read from across a room.
// gaze = camera position (-100..100) * GAIN / 100, clamped to +-CLAMP.
#define GAZE_GAIN_X 110
#define GAZE_GAIN_Y 70
#define GAZE_CLAMP_X 50
#define GAZE_CLAMP_Y 30
#define GAZE_MIRROR_X 1
#define GAZE_SMOOTHING 3     // move 1/N of the remaining distance per tick
#define FACE_SHIFT_PCT 60    // whole face moves this % of the gaze
#define PUPIL_R 11
#define PUPIL_TRAVEL_X 15    // max pupil offset inside the eye
#define PUPIL_TRAVEL_Y 18
#define TURN_SQUASH 12       // near eye grows / far eye shrinks by up to this
// "Noticing": when a face appears after a while with nobody there.
#define NOTICE_AFTER_ABSENT_MS 3000
#define NOTICE_MS 600
// Idle glances when nobody is in view.
#define IDLE_GLANCES 0  // off: no actions when nobody's found
#define WANDER_MIN_MS 2500
#define WANDER_RAND_MS 3000

#define EYES_Y 20
#define EYES_H 140
#define MOUTH_Y 165
#define MOUTH_H 70
#define EYE_OFFSET_X 55
#define EYE_CENTER_Y 70
#define MOUTH_CY 35

typedef enum {
  MOUTH_TEXT,
  MOUTH_SMILE,
  MOUTH_FLAT,
  MOUTH_DOTS,
  MOUTH_O,
  MOUTH_TALK,
} mouth_shape_t;

typedef struct {
  int dx, dy, w, h_left, h_right;
  uint16_t color;
  int brows;
  int pupils;          // draw pupils (when following a face)
  int pupil_x, pupil_y;  // pupil offset from eye centre, pixels
} eyes_t;

typedef struct {
  mouth_shape_t shape;
  int param;  // talk height or active dot
  uint16_t color;
  const char *text;  // MOUTH_TEXT caption (static strings, compared by pointer)
  int dx;            // horizontal shift, so the whole face turns together
} mouth_t;

static M5Canvas eyes_canvas(&M5.Display);
static M5Canvas mouth_canvas(&M5.Display);
static int screen_w = 320;

static uint16_t c_cyan, c_green, c_amber, c_amber_dim, c_purple, c_grey, c_white;

static const int talk_heights[] = {10, 26, 18, 32, 14, 24, 8, 30, 20, 12, 28};
static const int scan_offsets[] = {0, 10, 20, 28, 32, 28, 20, 10, 0, -10, -20, -28, -32, -28, -20, -10};

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static bool eyes_equal(const eyes_t &a, const eyes_t &b) {
  return a.dx == b.dx && a.dy == b.dy && a.w == b.w && a.h_left == b.h_left &&
         a.h_right == b.h_right && a.color == b.color && a.brows == b.brows &&
         a.pupils == b.pupils && a.pupil_x == b.pupil_x && a.pupil_y == b.pupil_y;
}

static bool mouth_equal(const mouth_t &a, const mouth_t &b) {
  return a.shape == b.shape && a.param == b.param && a.color == b.color &&
         a.text == b.text && a.dx == b.dx;
}

static void draw_eye(int cx, int cy, int w, int h, uint16_t color) {
  int r = (w < h ? w : h) / 3;
  if (r < 2) r = 2;
  eyes_canvas.fillSmoothRoundRect(cx - w / 2, cy - h / 2, w, h, r, color);
}

static void draw_eyes(const eyes_t &e) {
  int cx = screen_w / 2 + e.dx;
  int cy = EYE_CENTER_Y + e.dy;
  eyes_canvas.fillScreen(TFT_BLACK);
  draw_eye(cx - EYE_OFFSET_X, cy, e.w, e.h_left, e.color);
  draw_eye(cx + EYE_OFFSET_X, cy, e.w, e.h_right, e.color);
  if (e.pupils && e.h_left > 3 * PUPIL_R && e.h_right > 3 * PUPIL_R) {
    for (int side = -1; side <= 1; side += 2) {
      eyes_canvas.fillSmoothCircle(cx + side * EYE_OFFSET_X + e.pupil_x,
                                   cy + e.pupil_y, PUPIL_R, (uint16_t)TFT_BLACK);
    }
  }
  if (e.brows) {
    // Raised eyebrows: the "oh!" in "oh, go on".
    int bw = e.w + 8;
    int by = cy - (e.h_left > e.h_right ? e.h_left : e.h_right) / 2 - 22;
    eyes_canvas.fillSmoothRoundRect(cx - EYE_OFFSET_X - bw / 2, by, bw, 9, 4, e.color);
    eyes_canvas.fillSmoothRoundRect(cx + EYE_OFFSET_X - bw / 2, by, bw, 9, 4, e.color);
  }
  eyes_canvas.pushSprite(0, EYES_Y);
}

static void draw_mouth(const mouth_t &m) {
  int cx = screen_w / 2 + m.dx;
  int cy = MOUTH_CY;
  mouth_canvas.fillScreen(TFT_BLACK);

  switch (m.shape) {
    case MOUTH_TEXT:
      mouth_canvas.setTextColor(m.color);
      mouth_canvas.setTextDatum(middle_center);
      mouth_canvas.setTextSize(2);
      mouth_canvas.drawString(m.text ? m.text : "", cx, cy);
      break;
    case MOUTH_SMILE:
      mouth_canvas.fillArc(cx, cy - 16, 26, 32, 30, 150, m.color);
      break;
    case MOUTH_FLAT:
      mouth_canvas.fillSmoothRoundRect(cx - 22, cy - 3, 44, 7, 3, m.color);
      break;
    case MOUTH_DOTS:
      for (int i = 0; i < 3; i++) {
        mouth_canvas.fillSmoothCircle(cx - 24 + i * 24, cy, 6,
                                      i == m.param ? m.color : c_amber_dim);
      }
      break;
    case MOUTH_O:
      mouth_canvas.fillSmoothCircle(cx, cy, 14, m.color);
      mouth_canvas.fillSmoothCircle(cx, cy, 7, (uint16_t)TFT_BLACK);
      break;
    case MOUTH_TALK: {
      int h = m.param;
      int r = h / 2 < 10 ? h / 2 : 10;
      mouth_canvas.fillSmoothRoundRect(cx - 22, cy - h / 2, 44, h, r, m.color);
      break;
    }
  }
  mouth_canvas.pushSprite(0, MOUTH_Y);
}

static void face_task(void *) {
  face_state_t last_raw = (face_state_t)-1;
  face_state_t last_shown = (face_state_t)-1;
  int64_t raw_since = 0;
  int64_t shown_since = 0;
  int64_t next_blink = now_ms() + 3000;
  int gaze_x = 0, gaze_y = 0;
  bool was_present = false;
  int64_t absent_since = 0, notice_until = 0, next_wander = 0;
  int wander_idx = 0, wander_x = 0;
  uint32_t tick = 0;
  eyes_t last_eyes = {};
  mouth_t last_mouth = {};
  bool first = true;

  while (1) {
    int64_t now = now_ms();

    face_state_t raw = pipecat_face_state;
    if (raw != last_raw) {
      last_raw = raw;
      raw_since = now;
    }

    // "Oh, go on" is a reaction, not a resting state: hold it briefly, then
    // go back to listening while the user carries on.
    face_state_t shown = raw;
    if (raw == FACE_HESITATING && now - raw_since > HESITATE_HOLD_MS) {
      shown = FACE_LISTENING;
    }

    // Flux can flip eager/resumed within ~100ms while a word trails off.
    // Hold each expression briefly so those blips don't flicker; a resume
    // that comes back before the hold ends is never shown.
    // "Oh, go on" is the demo moment and happens while the user is talking,
    // so it gets a longer minimum than the other expressions.
    bool urgent = shown == FACE_SPEAKING || shown == FACE_CONNECTING;
    int64_t min_dwell =
        last_shown == FACE_HESITATING ? HESITATE_MIN_MS : MIN_DWELL_MS;
    if (shown != last_shown && !urgent && now - shown_since < min_dwell) {
      shown = last_shown;
    }
    if (shown != last_shown) {
      ESP_LOGI(LOG_TAG, "Face shown: %s", face_state_name(shown));
      last_shown = shown;
      shown_since = now;
    }

    bool blinking = false;
    if (now >= next_blink) {
      if (now < next_blink + BLINK_MS) {
        blinking = true;
      } else {
        next_blink = now + 2500 + esp_random() % 2500;
      }
    }

    // Gaze target: the tracked face, or idle glances when nobody's there.
    bool present = pipecat_face_present;
    if (present && !was_present && now - absent_since > NOTICE_AFTER_ABSENT_MS) {
      notice_until = now + NOTICE_MS;
    }
    if (!present && was_present) {
      absent_since = now;
    }
    was_present = present;

    int target_gx = 0, target_gy = 0;
    if (present) {
      target_gx = pipecat_face_target_x * GAZE_GAIN_X / 100;
      if (GAZE_MIRROR_X) target_gx = -target_gx;
      target_gy = pipecat_face_target_y * GAZE_GAIN_Y / 100;
    } else if (IDLE_GLANCES) {
      if (now >= next_wander) {
        static const int glances[] = {0, -35, 0, 30, 0, -20, 25, 0};
        wander_idx = (wander_idx + 1) % (int)(sizeof(glances) / sizeof(glances[0]));
        wander_x = glances[wander_idx];
        next_wander = now + WANDER_MIN_MS + esp_random() % WANDER_RAND_MS;
      }
      target_gx = wander_x;
    }
    if (target_gx > GAZE_CLAMP_X) target_gx = GAZE_CLAMP_X;
    if (target_gx < -GAZE_CLAMP_X) target_gx = -GAZE_CLAMP_X;
    if (target_gy > GAZE_CLAMP_Y) target_gy = GAZE_CLAMP_Y;
    if (target_gy < -GAZE_CLAMP_Y) target_gy = -GAZE_CLAMP_Y;
    // Each eye redraw pushes an ~89KB sprite out of PSRAM, which starved the
    // speaker's mixing (glitchy audio while the eyes roamed). So: hold the
    // gaze still while the bot is speaking, and move it at most every other
    // tick (~12 redraws/s) otherwise.
    tick++;
    if (shown != FACE_SPEAKING && (tick & 1) == 0) {
      gaze_x += (target_gx - gaze_x) / GAZE_SMOOTHING;
      gaze_y += (target_gy - gaze_y) / GAZE_SMOOTHING;
      if (abs(target_gx - gaze_x) < GAZE_SMOOTHING) gaze_x = target_gx;
      if (abs(target_gy - gaze_y) < GAZE_SMOOTHING) gaze_y = target_gy;
    }
    bool follow_gaze = true;

    eyes_t eyes;
    mouth_t mouth;
    switch (shown) {
      case FACE_CONNECTING:
        eyes = {0, 0, 56, 6, 6, c_grey};
        follow_gaze = false;
        mouth = {MOUTH_TEXT, 0, c_grey, "connecting..."};
        blinking = false;
        break;
      case FACE_LISTENING:
        eyes = {0, -4, 62, 78, 78, c_green};
        mouth = {MOUTH_FLAT, 0, c_green};
        break;
      case FACE_THINKING:
        eyes = {14, -14, 52, 44, 54, c_amber};
        follow_gaze = false;
        mouth = {MOUTH_DOTS, (int)((now / DOTS_STEP_MS) % 3), c_amber};
        blinking = false;
        break;
      case FACE_HESITATING: {
        int bounce = now - shown_since < HESITATE_BOUNCE_MS ? -10 : 0;
        eyes = {0, 4 + bounce, 66, 84, 84, c_purple, 1};
        mouth = {MOUTH_O, 0, c_purple};
        blinking = false;
        break;
      }
      case FACE_LOOKING: {
        int n = sizeof(scan_offsets) / sizeof(scan_offsets[0]);
        int scan = scan_offsets[(now / SCAN_STEP_MS) % n];
        eyes = {scan, -8, 58, 52, 52, c_white};
        follow_gaze = false;
        mouth = {MOUTH_TEXT, 0, c_white, pipecat_face_caption};
        blinking = false;
        break;
      }
      case FACE_SPEAKING: {
        int n = sizeof(talk_heights) / sizeof(talk_heights[0]);
        eyes = {0, 0, 56, 64, 64, c_cyan};
        mouth = {MOUTH_TALK, talk_heights[(now / TALK_STEP_MS) % n], c_cyan};
        break;
      }
      case FACE_IDLE:
      default:
        eyes = {0, 0, 56, 64, 64, c_cyan};
        mouth = {MOUTH_SMILE, 0, c_cyan};
        break;
    }

    if (follow_gaze) {
      // The whole face turns part of the way, the pupils do the looking, and
      // the eye on the side it's turning towards grows (a 3D head turn).
      int shift_x = gaze_x * FACE_SHIFT_PCT / 100;
      eyes.dx += shift_x;
      eyes.dy += gaze_y * FACE_SHIFT_PCT / 100;
      if (mouth.shape != MOUTH_TEXT) {
        mouth.dx = shift_x;
      }
      eyes.pupils = 1;
      eyes.pupil_x = gaze_x * PUPIL_TRAVEL_X / GAZE_CLAMP_X;
      eyes.pupil_y = gaze_y * PUPIL_TRAVEL_Y / GAZE_CLAMP_Y;
      int squash = gaze_x * TURN_SQUASH / GAZE_CLAMP_X;
      eyes.h_left -= squash;
      eyes.h_right += squash;

      if (now < notice_until) {
        // "Oh, hello!": eyes pop wider with a little hop.
        int hop = now < notice_until - NOTICE_MS / 2 ? -8 : 0;
        eyes.w += 8;
        eyes.h_left += 14;
        eyes.h_right += 14;
        eyes.dy += hop;
      }
    }

    if (blinking) {
      eyes.h_left = 6;
      eyes.h_right = 6;
    }

    if (first || !eyes_equal(eyes, last_eyes)) {
      draw_eyes(eyes);
      last_eyes = eyes;
    }
    if (first || !mouth_equal(mouth, last_mouth)) {
      draw_mouth(mouth);
      last_mouth = mouth;
    }
    first = false;

    vTaskDelay(pdMS_TO_TICKS(FACE_TICK_MS));
  }
}

void pipecat_face_init() {
#if FACE_TRACK_PREVIEW
  return;  // the camera preview owns the display
#endif
  screen_w = M5.Display.width();
  M5.Display.fillScreen(TFT_BLACK);

  c_cyan = M5.Display.color565(70, 210, 255);
  c_green = M5.Display.color565(90, 255, 160);
  c_amber = M5.Display.color565(255, 190, 60);
  c_amber_dim = M5.Display.color565(110, 80, 25);
  c_purple = M5.Display.color565(185, 110, 255);
  c_grey = M5.Display.color565(90, 90, 90);
  c_white = M5.Display.color565(225, 235, 255);

  eyes_canvas.setPsram(true);
  eyes_canvas.setColorDepth(16);
  eyes_canvas.createSprite(screen_w, EYES_H);

  mouth_canvas.setPsram(true);
  mouth_canvas.setColorDepth(16);
  mouth_canvas.createSprite(screen_w, MOUTH_H);

  // Above face detection (priority 1) so the face stays smooth while a
  // detection runs.
  xTaskCreatePinnedToCore(face_task, "face", 6144, NULL, 2, NULL, 1);
}
