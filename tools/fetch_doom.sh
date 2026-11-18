#!/usr/bin/env bash
# Fetch what Doom needs:
#  - the doomgeneric engine source (GPL-2). A copy is already in third_party/doomgeneric
#    (upstream commit e257a09); it is cloned only if that folder is missing.
#  - the shareware DOOM1.WAD v1.9 (not kept in this repository), copied to the drive with:
#      python tools/bv.py put third_party/wad/doom1.wad /disk/Apps/doom1.wad
#    (or copy it to the root of the SD card with a computer)
cd "$(dirname "$0")/.."
mkdir -p third_party/wad
[ -d third_party/doomgeneric ] || git clone --depth 1 https://github.com/ozkl/doomgeneric.git third_party/doomgeneric
[ -f third_party/wad/doom1.wad ] || curl -sL -o third_party/wad/doom1.wad https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad
if command -v md5 >/dev/null; then sum=$(md5 -q third_party/wad/doom1.wad); else sum=$(md5sum third_party/wad/doom1.wad | cut -d' ' -f1); fi
[ "$sum" = "f0cefca49926d00903cf57551d901abe" ] && echo "doom1.wad OK (shareware v1.9)" || echo "doom1.wad checksum mismatch"
