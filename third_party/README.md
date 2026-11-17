# Third-party code

## doomgeneric

`third_party/doomgeneric/` is a copy of [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric)
at commit `e257a09527062bfb986b6e5e0780839b6cbd5b71`, the Doom engine (based on Chocolate Doom)
reduced to a few platform functions. No source file is changed; the upstream `screenshots/`
folder was left out to save space.

- Licence: **GNU GPL v2** (`doomgeneric/LICENSE`; the source headers say "version 2, or (at your
  option) any later version").
- Built into `disk/Apps/<target>/Doom.elf` together with `apps/doom/doom_bevel.c` by
  `tools/build_doom.sh esp32p4` or `tools/build_doom.sh esp32s3`. The copy in `disk/Apps/esp32s3/`
  was built before the two-board split, so it is the older Xtensa build of the same source.
  `Doom.elf` as a whole is therefore covered by the GPL v2, and its complete source is in this
  repository (this folder, `apps/doom/`, `apps/crt.c`, `apps/bevel.h` and the build script).

## Not included

- **DOOM1.WAD** (the shareware game data, id Software). It is not committed. `tools/fetch_doom.sh`
  downloads it to `third_party/wad/doom1.wad` and checks its MD5
  (`f0cefca49926d00903cf57551d901abe`, shareware v1.9). Any copy of the shareware v1.9 `doom1.wad`
  works.
- **ESP-IDF components** `espressif/elf_loader` and `espressif/usb_host_hid` (Apache-2.0). The
  ESP-IDF component manager downloads them into `managed_components/` on the first build, at the
  versions pinned in `dependencies.lock`.
