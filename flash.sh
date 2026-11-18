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
export LOG_DATACHANNEL_MESSAGES=1

# ESP-IDF 5.4.1 was installed with Python 3.12; the default python3 is 3.15.
export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH"
source ~/esp/esp-idf/export.sh >/dev/null

cd "$ROOT/pipecat-esp32/esp32-m5stack-cores3"
echo "server: $PIPECAT_SMALLWEBRTC_URL"

case "${1:-all}" in
  build) idf.py build ;;
  flash) idf.py build flash ;;
  monitor) idf.py monitor ;;
  all) idf.py build flash monitor ;;
esac
