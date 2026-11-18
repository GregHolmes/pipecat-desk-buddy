#!/usr/bin/env zsh
# Reset the CoreS3 and (re)start the background serial log capture.
# Wi-Fi SSID lines are filtered out before anything is written.
ROOT=${0:A:h:h}
LOG=${SERIAL_LOG:-/private/var/folders/v9/6bdxf7bx7fj967wbg6x_jdrr0000gn/T/opencode/serial.log}
PY=~/.espressif/python_env/idf5.4_py3.12_env/bin/python

pkill -f serial_log.py
sleep 0.5
nohup zsh -c "$PY -u '$ROOT/tools/serial_log.py' 3600 2>&1 | grep --line-buffered -v 'connected with\|SSID\|ssid' > '$LOG'" >/dev/null 2>&1 &
echo "Reset sent, logging to $LOG"
