# Desk Buddy

A little voice companion that lives on your desk: an **M5Stack CoreS3** with an animated face, powered by **[Pipecat](https://github.com/pipecat-ai/pipecat)** with **Deepgram Flux** for listening and speaking and **Claude** as the brain.

It reacts to the conversation as it happens. It looks like it's thinking when you pause, goes "oh, go on" when you carry on mid-sentence, looks things up on the web, and follows you with its eyes using on-device face tracking.

Built in a day at the Pipecat hackathon (18 Nov 2026).

> Demo video: _coming soon_

## What it does

| You see | When | Driven by |
|---|---|---|
| Cyan eyes, smile, blinking | Idle | Default state |
| Green eyes, flat mouth | You're talking | RTVI `user-started-speaking` (Flux start of turn) |
| Amber eyes looking up, "..." | You paused, it might be your turn | Flux **eager end of turn**, sent as an RTVI `server-message` |
| Purple wide eyes, raised brows, "o" mouth | "Oh, go on": you carried on after a pause | Flux turn resumed, or a turn Claude judged incomplete |
| White scanning eyes + caption | Looking something up | RTVI `llm-function-call-started` / `-stopped` |
| Moving mouth | It's speaking | RTVI `bot-started-speaking` / `bot-stopped-speaking` |
| Pupils and face turn towards you | You move around | On-device face detection (camera + esp-dl) |

Also:

- **Tools:** current weather anywhere (Open-Meteo), the time in any timezone, and **web search** (Claude with Anthropic's web search tool).
- **Tap to interrupt:** the CoreS3 is half duplex (mic off while it speaks), so tapping the screen sends an RTVI `client-message` and the bot stops.
- **Hesitation-aware turn taking:** "I was wondering..." *(pause)* "...what's the best way to make tea?" is treated as one turn. Flux's eager end of turn starts a speculative reply, and Pipecat's LLM turn-completion gate holds turns Claude marks as incomplete.

## How it works

```
 M5Stack CoreS3                                Pipecat bot (server/bot.py)
 ┌──────────────────────────┐   WebRTC     ┌──────────────────────────────────────────┐
 │ mic ──── Opus ───────────┼────────────► │ Deepgram Flux STT  (turns, eager EOT)    │
 │                          │              │        │                                 │
 │ speaker ◄── Opus ────────┼◄──────────── │ Claude (Anthropic) + tools               │
 │                          │              │        │   weather / time / web search   │
 │ face (display task) ◄────┼◄── RTVI ──── │ Deepgram Flux TTS                        │
 │ tap / ready ─────────────┼─── RTVI ───► │ RTVI observer + custom face events       │
 │ camera → face detection  │              └──────────────────────────────────────────┘
 │ (on device, nothing sent)│
 └──────────────────────────┘
```

- **Transport:** Pipecat SmallWebRTC. The device posts its SDP offer to `/api/offer`, then audio flows as Opus and events flow over the RTVI data channel.
- **Face events** come from Pipecat's RTVI observer (user/bot speaking, function calls) plus two custom `server-message`s from Flux handlers (`eager_eot`, `turn_resumed`).
- **Face tracking** runs entirely on the CoreS3: GC0308 camera, Espressif `human_face_detect` (esp-dl), a couple of detections a second, only while idle or listening so it never competes with playback.

## Repository layout

```
server/            Pipecat bot: bot.py (pipeline), tools.py (weather, time, web search)
firmware/          CoreS3 firmware (our modified copy of pipecat-esp32's CoreS3 board dir)
  src/face.cpp       animated face, gaze, expressions
  src/face_track.cpp camera + on-device face detection
  patches/           fixes to upstream libpeer, applied by flash.sh
third_party/       pipecat-esp32 upstream as a submodule (pinned), for its shared components
tools/             bot.sh (run the bot), reset.sh + serial_log.py (reset and log the device)
flash.sh           build + flash the firmware
docs/              FRICTION_LOG.md (feedback for the Pipecat team), HANDOVER.md (plan and status)
```

## Running it

### You'll need

- M5Stack **CoreS3** (with camera) and a USB-C cable
- A Mac (or Linux machine) on the same network, with **2.4GHz Wi-Fi** reachable by the CoreS3
- A **Deepgram** API key and an **Anthropic** API key (with web search enabled for the search tool)

### 1. Server

```bash
git clone --recurse-submodules https://github.com/GregHolmes/pipecat-desk-buddy.git
cd pipecat-desk-buddy
cd server
cp .env.example .env        # add DEEPGRAM_API_KEY and ANTHROPIC_API_KEY
uv sync                     # Python 3.12, Pipecat 1.16 with deepgram, anthropic, webrtc, runner
cd .. && tools/bot.sh       # runs: uv run bot.py -t webrtc --esp32 --host <your LAN IP>
```

`tools/bot.sh` force-stops any previous bot first (a plain SIGTERM leaves the old WebRTC session running stale code). Without the device you can try `uv run bot.py` and Pipecat's browser client (not tested during the hackathon).

Optional settings (env vars, defaults in `server/.env.example`): LLM model, Flux TTS voice and expressivity, `DESK_BUDDY_TTS=aura` to switch to Aura-2, and the Flux end-of-turn thresholds.

### 2. Firmware

Install **ESP-IDF v5.4.1** (with Python 3.12; newer Pythons don't work with this IDF):

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.1 --recursive --shallow-submodules --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH" ./install.sh esp32s3
```

Then, from the repo root:

```bash
git submodule update --init --recursive
cp .wifi.env.example .wifi.env   # 2.4GHz SSID and password
./flash.sh                       # build, flash, open the serial monitor
```

`flash.sh` applies `firmware/patches` to the pinned libpeer, points the firmware at `http://<your IP>:7860/api/offer`, and always reconfigures (the Wi-Fi details and server URL are baked in at configure time).

Build switches (env vars for `flash.sh`): `FACE_TRACK_OFF=1` (no camera), `FACE_TRACK_PREVIEW=1` (show the camera image with detection boxes), `LOG_DC=1` (log every RTVI message).

## What we had to fix (and whose code it was)

The CoreS3 firmware worked out of the box, but adding a reactive face exposed several bugs. All are fixed here (`firmware/`, `firmware/patches/`, `server/`) and written up in [docs/FRICTION_LOG.md](docs/FRICTION_LOG.md).

Where the code lives:

- **pipecat-esp32** is Pipecat's own firmware ([pipecat-ai/pipecat-esp32](https://github.com/pipecat-ai/pipecat-esp32), MIT, Daily; the CoreS3 board support was added in July 2025). Most of the bugs below are here.
- **libpeer** is the third-party WebRTC stack ([sepfy/libpeer](https://github.com/sepfy/libpeer)) that pipecat-esp32 pulls in through a Pipecat maintainer's fork ([aconchillo/libpeer](https://github.com/aconchillo/libpeer)). Not Pipecat's code, but Pipecat's dependency.
- **M5Unified** is M5Stack's hardware library. Its behaviour is documented; the problems came from how pipecat-esp32 uses it.
- **Pipecat** itself ([pipecat-ai/pipecat](https://github.com/pipecat-ai/pipecat)), the server framework.
- Two rows were partly **our own doing** while building this, and are marked as such.

| Symptom | Cause | Where | Fix |
|---|---|---|---|
| Crackly bot audio | Every decoded frame went into one reused buffer passed to `M5.Speaker.playRaw()`, which doesn't copy; plus a 2x gain that clipped | pipecat-esp32 (`media.cpp`), misusing M5Unified | Ring buffer + feeder task on a fixed channel, no extra gain |
| RTVI events lost or 3 to 6s late | The built-in SCTP ignored chunk padding, so bundled messages were misread and mis-acknowledged | libpeer (`sctp.c`), shipped via Pipecat's fork | One-line fix in `firmware/patches` |
| Audio underruns, dropped events | `peer_connection_loop()` reads one UDP packet per call, and the firmware called it every 15ms (50+ packets/s arrive), with a 6-slot lwIP mailbox | pipecat-esp32 (`main.cpp`, `sdkconfig`), on libpeer's design | Loop every 1ms, mailbox of 32 |
| Mic audio ~37% short and partly torn | An extra 15ms sleep per 20ms frame, and one buffer reused with the asynchronous `M5.Mic.record()` | pipecat-esp32 (`webrtc.cpp`, `media.cpp`) | Real-time pacing, three rotating buffers (50 frames/s) |
| Device hung mid-call, never recovered | Mic shut down by one task while another was inside `M5.Mic.record()`; no keepalive or media watchdog | pipecat-esp32 (`media.cpp`, `webrtc.cpp`) | One task owns mic/speaker switching; audio watchdog reconnects |
| Device went deaf after sending a message | libpeer isn't thread-safe, and nothing says so. **We** introduced a second task sending on the data channel | libpeer (undocumented), triggered by our code | All sends go through the main loop |
| Choppy audio with Flux TTS | Flux TTS streams per word, so Pipecat sent one `bot-tts-text` per word; the firmware's RTVI handler blocks the WebRTC loop when its queue is full | Pipecat (RTVI observer defaults) + pipecat-esp32 (`rtvi.cpp`) | Non-blocking queue; the bot doesn't send `bot-tts-text` |
| Bot greeted before the device was ready | `client-ready` was never sent | pipecat-esp32 (`rtvi.cpp`) | Sent when the data channel opens; the bot greets on `on_client_ready` |
| Slow reconnects after reboots | Every failure path calls `esp_restart()` without disconnecting from the AP. **We** also made it worse by turning Wi-Fi power save off from boot | pipecat-esp32 (`http.cpp`, `webrtc.cpp`) + ours | Disconnect before restarts; power save off only once the call is up |

Pipecat server feedback (no workaround needed in pipecat-esp32, details in the friction log): no event when a turn is held as incomplete or resumed, `AnthropicLLMService` can't use Anthropic's server-side tools such as web search (we wrapped it as a function tool), and stopping the dev runner with SIGTERM leaves the active WebRTC session running old code.

## Known limitations

- **Half duplex:** M5Unified runs the CoreS3 mic and speaker on one I2S bus, so you can't interrupt by voice; tap the screen instead. Full duplex would need custom I2S drivers plus echo cancellation.
- **Reconnects:** some routers take 30 to 90s to pass traffic again if the device reconnects within seconds of its last session (for example after a flash). A cold power-on is fine.
- **Face tracking** needs decent light and the CoreS3 angled so your face is mid-frame (the 0.3MP GC0308 is small).
- **Presence lines are switched off.** The device can say "Oh, where have you gone?" when you walk out of view and "Oh, there you are!" when you come back (`PRESENCE_LINES` in `firmware/src/face_track.cpp`, spoken by the bot via `TTSSpeakFrame`). With it on, the face tracker sent those RTVI messages from its own FreeRTOS task while the WebRTC loop was using the connection. libpeer isn't thread-safe, so this corrupted the session: the device went deaf (no audio or events) right after "where have you gone?", and replies around it were garbled. Idle eye glances while nobody was in view added display load at the same moments. Sends now all go through the main loop, which should make it safe, but it wasn't re-tested before submission, so it stays off along with the idle glances (`IDLE_GLANCES` in `face.cpp`).

## Built with

- [Pipecat](https://github.com/pipecat-ai/pipecat) 1.16 and [pipecat-esp32](https://github.com/pipecat-ai/pipecat-esp32) (MIT, see `firmware/LICENSE-pipecat-esp32`)
- [Deepgram](https://deepgram.com) Flux STT and Flux TTS
- [Anthropic](https://www.anthropic.com) Claude (Haiku 5.5) with web search
- [libpeer](https://github.com/sepfy/libpeer) (MIT), [M5Unified](https://github.com/m5stack/M5Unified), Espressif [esp-dl](https://github.com/espressif/esp-dl) and [esp32-camera](https://github.com/espressif/esp32-camera)
- [Open-Meteo](https://open-meteo.com) for weather
