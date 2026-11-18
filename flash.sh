#!/usr/bin/env zsh
# Build, flash and monitor the CoreS3 firmware.
#
# Wi-Fi credentials come from the environment or from .wifi.env (gitignored):
#   WIFI_SSID=...
#   WIFI_PASSWORD=...
#
# Usage: ./flash.sh [build|flash|monitor]   (default: build + flash + monitor)

set -e
ROOT=${0:A:h}

[[ -f "$ROOT/.wifi.env" ]] && source "$ROOT/.wifi.env"
: ${WIFI_SSID:?Set WIFI_SSID in .wifi.env}
: ${WIFI_PASSWORD:?Set WIFI_PASSWORD in .wifi.env}

MAC_IP=${MAC_IP:-$(ipconfig getifaddr en0)}
export WIFI_SSID WIFI_PASSWORD
export PIPECAT_SMALLWEBRTC_URL="http://${MAC_IP}:7860/api/offer"
# Logs every RTVI message from the WebRTC loop: debug only (LOG_DC=1).
[[ -n "$LOG_DC" ]] && export LOG_DATACHANNEL_MESSAGES=1 || unset LOG_DATACHANNEL_MESSAGES

# ESP-IDF 5.4.1 was installed with Python 3.12; the default python3 is 3.15.
export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH"
source ~/esp/esp-idf/export.sh >/dev/null

# Our fixes to upstream libpeer (pinned in third_party/) live as patches.
LIBPEER="$ROOT/third_party/pipecat-esp32/esp32-s3-box-3/deps/libpeer"
for p in "$ROOT"/firmware/patches/*.patch; do
  if git -C "$LIBPEER" apply --check "$p" 2>/dev/null; then
    git -C "$LIBPEER" apply "$p" && echo "applied $(basename "$p")"
  elif ! git -C "$LIBPEER" apply --reverse --check "$p" 2>/dev/null; then
    echo "patch $(basename "$p") doesn't apply; is the submodule at the pinned commit?"; exit 1
  fi
done

cd "$ROOT/firmware"
echo "server: $PIPECAT_SMALLWEBRTC_URL"

# Only ever talk to the CoreS3, never other serial devices (e.g. headsets).
if [[ "${1:-all}" != build ]]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  [[ -n "$PORT" ]] || { echo "CoreS3 not found on USB (no /dev/cu.usbmodem*). Is it plugged in?"; exit 1; }
  export ESPPORT="$PORT"
fi

# The firmware's CMakeLists reads WIFI_* and PIPECAT_SMALLWEBRTC_URL only at
# configure time, so always reconfigure or stale values get baked in silently.
case "${1:-all}" in
  build) idf.py reconfigure build ;;
  flash) idf.py reconfigure build flash ;;
  monitor) idf.py monitor ;;
  all) idf.py reconfigure build flash monitor ;;
esac
