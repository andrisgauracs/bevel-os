#!/usr/bin/env bash
# Build Doom (third_party/doomgeneric + apps/doom/doom_bevel.c) into disk/Apps/<target>/Doom.elf.
# Run from a shell where ESP-IDF is active. Fetch the engine and the WAD first (tools/fetch_doom.sh).
#   tools/build_doom.sh            -> esp32p4 (RISC-V)
#   tools/build_doom.sh esp32s3    -> the ESP32-S3 VGA devkit (Xtensa)
TARGET=${1:-esp32p4}
cd "$(dirname "$0")/.." || exit 1
case "$TARGET" in
  esp32p4)
    CC=riscv32-esp-elf-gcc
    STRIP=riscv32-esp-elf-strip
    ARCH="-march=rv32imafc_zicsr_zifencei -mabi=ilp32f"
    EXTRA="--remove-section=.riscv.attributes"
    DEFINES="-DBV_ARCH_RISCV=1 -DFEATURE_SOUND"      # sound effects through the OS mixer
    LIBS=""       # no libgcc: the RISC-V one is not position-independent, the OS exports those routines
    INCLUDES="-Iapps/doom"   # the empty SDL_mixer.h FEATURE_SOUND wants
    ;;
  esp32s3)
    CC=xtensa-esp32s3-elf-gcc
    STRIP=xtensa-esp32s3-elf-strip
    ARCH="-mlongcalls"
    EXTRA="--remove-section=.xt.lit --remove-section=.xt.prop --remove-section=.xtensa.info"
    DEFINES="-DBV_ARCH_RISCV=0"                      # no sound hardware on this board
    # no libgcc either: linked into this shared ELF, its non-PIC __divdi3 (m_fixed.c's FixedDiv)
    # makes ld assert (elf32-xtensa.c:3301); the OS exports those routines (app_loader.c)
    LIBS=""
    INCLUDES=""
    ;;
  *) echo "target must be esp32p4 or esp32s3"; exit 1 ;;
esac
SRC=third_party/doomgeneric/doomgeneric
OBJ=build/doom-$TARGET
OUT=disk/Apps/$TARGET
mkdir -p $OBJ $OUT
FILES="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game
 hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config
 m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj
 p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane
 r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main
 w_wad z_zone w_file_stdc i_input i_video doomgeneric"
CFLAGS="$ARCH -O2 -w -fPIC -fdata-sections -ffunction-sections -fvisibility=hidden -fno-jump-tables
 -DCMAP256 $DEFINES -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX -D_DEFAULT_SOURCE
 $INCLUDES -I$SRC -Iapps"
objs=""
for f in $FILES; do
  o=$OBJ/$f.o
  if [ ! -f $o ] || [ $SRC/$f.c -nt $o ]; then $CC $CFLAGS -c $SRC/$f.c -o $o || exit 1; fi
  objs="$objs $o"
done
$CC $CFLAGS -Wall -c apps/doom/doom_bevel.c -o $OBJ/doom_bevel.o || exit 1
$CC $CFLAGS -c apps/crt.c -o $OBJ/crt.o || exit 1
$CC $ARCH -shared -fPIC -nostartfiles -nostdlib -e bevel_entry \
    -Wl,--gc-sections -Wl,--strip-all -Wl,--strip-debug $OBJ/crt.o $OBJ/doom_bevel.o $objs $LIBS -o $OUT/Doom.elf || exit 1
$STRIP --strip-unneeded --remove-section=.comment $EXTRA $OUT/Doom.elf
python tools/elf_fix.py $OUT/Doom.elf
printf "Doom.elf %d bytes\n" "$(wc -c < $OUT/Doom.elf)"
