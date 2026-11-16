#!/bin/bash
# Runs Bevel OS in QEMU (Espressif's qemu-system-xtensa, machine esp32s3): the real firmware,
# Xtensa apps and all, built with sdkconfig.qemu (headless, VNC, emulated Ethernet).
#
#   tools/qemu.sh [--no-build] [--wipe] [--vnc-port P] [--serial-port P]
#
# Then: a VNC viewer on 127.0.0.1:5900 for the desktop, and
#       BV_PORT=tcp:127.0.0.1:5555 tools/bv.py daemon   for the serial channel (dumps, tests).
# The drive (the FAT partition) is kept between runs in build-qemu/qemu_flash.bin; --wipe
# starts from a blank one. Ctrl+C stops the emulator.
set -e
cd "$(dirname "$0")/.."
BUILD=1; WIPE=0; VNC_PORT=5900; SERIAL_PORT=5555
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --wipe) WIPE=1 ;;
    --vnc-port) VNC_PORT=$2; shift ;;
    --serial-port) SERIAL_PORT=$2; shift ;;
    *) sed -n '2,11p' "$0"; exit 2 ;;
  esac
  shift
done

IDF=${IDF_PATH:-$HOME/esp/esp-idf}
. "$IDF/export.sh" >/dev/null 2>&1
QEMU=$(command -v qemu-system-xtensa || ls -d "$HOME"/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | tail -1)
[ -x "$QEMU" ] || { echo "qemu-system-xtensa not found: python \$IDF_PATH/tools/idf_tools.py install qemu-xtensa"; exit 1; }
# QEMU needs libslirp for its user-mode network; without root it can live in ~/esp/qemu-libs
# (apt-get download libslirp0 && dpkg-deb -x libslirp0_*.deb ~/esp/qemu-libs/root)
export LD_LIBRARY_PATH="$HOME/esp/qemu-libs/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

B=build-qemu
if [ $BUILD = 1 ]; then
  idf.py -B $B -D SDKCONFIG=$B/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build \
    | grep -E "(error|warning):|binary size|FAILED" || true
  [ ${PIPESTATUS[0]} -eq 0 ] || { echo "build failed"; exit 1; }
fi

# Flash image: bootloader + partition table + app, keeping the drive of the previous run.
IMG=$B/qemu_flash.bin
STORAGE_OFF=$((0x310000)); STORAGE_LEN=$((0xC00000))
if [ -f $IMG ] && [ $WIPE = 0 ]; then
  dd if=$IMG of=$B/storage.keep bs=4096 skip=$((STORAGE_OFF / 4096)) count=$((STORAGE_LEN / 4096)) status=none
fi
(cd $B && python -m esptool --chip esp32s3 merge_bin --output qemu_flash.bin --fill-flash-size 16MB @flash_args >/dev/null)
if [ -f $B/storage.keep ] && [ $WIPE = 0 ]; then
  dd if=$B/storage.keep of=$IMG bs=4096 seek=$((STORAGE_OFF / 4096)) conv=notrunc status=none
  rm -f $B/storage.keep
fi

# eFuses of a chip revision 0.3 ESP32-S3 (as idf.py qemu does)
EFUSE=$B/qemu_efuse.bin
[ -f $EFUSE ] || python3 - "$EFUSE" <<'EOF'
import sys
data = bytearray(1024 + 24)
data[38] = 0x0c
open(sys.argv[1], "wb").write(bytes(data))
EOF

echo "Bevel OS in QEMU: VNC 127.0.0.1:$VNC_PORT, serial channel tcp:127.0.0.1:$SERIAL_PORT (Ctrl+C to stop)"
exec "$QEMU" -M esp32s3 -m 8M \
  -drive file=$IMG,if=mtd,format=raw \
  -drive file=$EFUSE,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
  -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
  -global driver=ssi_psram,property=is_octal,value=true \
  -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:$VNC_PORT-:5900 \
  -serial tcp:127.0.0.1:$SERIAL_PORT,server,nowait \
  -nographic -monitor none
