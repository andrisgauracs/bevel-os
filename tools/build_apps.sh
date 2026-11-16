#!/usr/bin/env bash
# Build every apps/<Name>.c into disk/Apps/<Name>.elf (a position-independent ELF for the loader).
# An app needs nothing but apps/bevel.h. Needs the ESP-IDF RISC-V toolchain on the PATH
# (riscv32-esp-elf-gcc: run this from a shell where ESP-IDF is active).
cd "$(dirname "$0")/.." || exit 1
CC=riscv32-esp-elf-gcc
ARCH="-march=rv32imafc_zicsr_zifencei -mabi=ilp32f"
mkdir -p disk/Apps
for src in apps/*.c; do
  name=$(basename "$src" .c)
  [ "$name" = "crt" ] && continue
  out=disk/Apps/$name.elf
  $CC $ARCH -Os -Wall -fPIC -shared -nostartfiles -nostdlib -e bevel_entry \
      -fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden \
      -Wl,--strip-all -Wl,--strip-debug -Iapps apps/crt.c "$src" -lgcc -o "$out" || exit 1
  riscv32-esp-elf-strip --strip-unneeded --remove-section=.comment --remove-section=.dynamic \
      --remove-section=.riscv.attributes "$out"
  printf "%-14s %6d bytes\n" "$name.elf" "$(wc -c < "$out")"
done
