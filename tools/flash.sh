#!/bin/zsh
# Build, flash over the COM port and restart the serial daemon. Usage: tools/flash.sh [extra idf.py args]
cd "$(dirname "$0")/.."
export IDF_PRESET=${IDF_PRESET:-esp32s3}   # the CMake preset, i.e. the board (this was the S3 machine)
PY=~/.espressif/python_env/idf5.5_py3.12_env/bin/python
PORT=${BV_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)}
. ~/esp/esp-idf/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "(error|warning):|undefined reference|bevel_os.bin binary size" | cut -c1-260 | sort -u
[ ${pipestatus[1]} -ne 0 ] && { echo "BUILD FAILED"; exit 1; }
$PY tools/bv.py quit >/dev/null 2>&1
sleep 0.3
idf.py -p $PORT -b 921600 flash 2>&1 | grep -E "Hash of data verified|error|Error|Hard resetting" | tail -4
: > build/serial.log
nohup $PY tools/bv.py daemon >/dev/null 2>&1 &
sleep 2.5
$PY tools/bv.py log 30
