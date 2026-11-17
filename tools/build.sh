#!/bin/zsh
# Build and print only compiler diagnostics and the size line.
cd "$(dirname "$0")/.."
export IDF_PRESET=${IDF_PRESET:-esp32s3}   # the CMake preset, i.e. the board (this was the S3 machine)
. ~/esp/esp-idf/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "(error|warning):|undefined reference|bevel_os.bin binary size|ninja: build stopped" | cut -c1-260 | sort -u | head -${1:-40}
