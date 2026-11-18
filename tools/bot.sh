#!/usr/bin/env zsh
# (Re)start the Desk Buddy bot in ESP32 mode, in the foreground.
#
# SIGTERM isn't enough: uvicorn stops listening but keeps an active WebRTC
# session alive, so the device stays on the old code. Kill it outright; the
# device's audio watchdog then reboots it onto the new bot.
ROOT=${0:A:h:h}
MAC_IP=${MAC_IP:-$(ipconfig getifaddr en0)}

pkill -9 -f "bot.py -t webrtc" && sleep 1
cd "$ROOT/server"
exec uv run bot.py -t webrtc --esp32 --host "$MAC_IP"
