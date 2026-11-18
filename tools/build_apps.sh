#!/usr/bin/env bash
# Build every apps/<Name>.c into disk/Apps/<target>/<Name>.elf (a position-independent ELF for the
# loader). An app needs nothing but apps/bevel.h. Run from a shell where ESP-IDF is active, so the
# toolchain is on the PATH; the target only picks the compiler, not the SDK.
#   tools/build_apps.sh            -> esp32p4 (RISC-V, the JC1060P470-V2 board)
#   tools/build_apps.sh esp32s3    -> the ESP32-S3 VGA devkit (Xtensa)
TARGET=${1:-esp32p4}
cd "$(dirname "$0")/.." || exit 1
case "$TARGET" in
  esp32p4)
    CC=riscv32-esp-elf-gcc
    STRIP=riscv32-esp-elf-strip
    ARCH="-march=rv32imafc_zicsr_zifencei -mabi=ilp32f"
    EXTRA="--remove-section=.riscv.attributes"
    DEFINES="-DBV_ARCH_RISCV=1"
    ;;
  esp32s3)
    CC=xtensa-esp32s3-elf-gcc
    STRIP=xtensa-esp32s3-elf-strip
    ARCH="-mlongcalls"
    EXTRA="--remove-section=.xt.lit --remove-section=.xt.prop --remove-section=.xtensa.info"
    DEFINES="-DBV_ARCH_RISCV=0"
    ;;
  *) echo "target must be esp32p4 or esp32s3"; exit 1 ;;
esac
OUT=disk/Apps/$TARGET
mkdir -p "$OUT"
for src in apps/*.c; do
  name=$(basename "$src" .c)
  [ "$name" = "crt" ] && continue
  out="$OUT/$name.elf"
  $CC $ARCH -Os -Wall -fPIC -shared -nostartfiles -nostdlib -e bevel_entry $DEFINES \
      -fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden \
      -Wl,--strip-all -Wl,--strip-debug -Iapps apps/crt.c "$src" -lgcc -o "$out" || exit 1
  $STRIP --strip-unneeded --remove-section=.comment --remove-section=.dynamic $EXTRA "$out"
  printf "%-14s %6d bytes\n" "$name.elf" "$(wc -c < "$out")"
done
