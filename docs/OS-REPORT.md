# Bevel OS: what was built

Bevel OS is a small graphical desktop operating system for an ESP32-S3 (N16R8) driving a VGA
monitor through six resistors. It has an original mid-90s look (grey bevelled panels, navy title
bars, a taskbar with a menu button), its own bitmap fonts and pixel-art icons, and can be used
entirely with the keyboard or entirely with the mouse. It sits on ESP-IDF 5.5.3 and FreeRTOS for
the boot chain, USB host, FAT and scheduling; everything visible is written from scratch.

## Checklist

| # | Requirement | State | How it was checked |
|---|---|---|---|
| 1 | Boots straight to a desktop with a mouse cursor | Done, 0.7 s | serial log, framebuffer dump, JetKVM |
| 2 | Windows drag, focus, minimise to the taskbar, close | Done | JetKVM with the real mouse; `tools/kbtest.py` |
| 3 | File browser | Done, on the internal flash drive | `kbtest.py` (folder, file, rename, copy, delete) |
| 4 | Text editor that types and saves | Done | `kbtest.py` compares the saved file byte for byte |
| 5 | Launches a separate app binary, a game | Done: `Snake.elf` loaded with `elf_loader` | `kbtest.py`, framebuffer dump |
| 6 | All of it keyboard-only and mouse-only | Keyboard: 34/34 scripted steps. Mouse: checked by hand through the JetKVM | |

**Change of plan:** the human decided not to use an SD card. Storage is a 12 MB wear-levelled FAT
drive in the board's own flash, mounted at `/disk`. Everything the brief asked of the card (files,
settings, loadable apps, free/total space) works on that drive. "Survives the card being removed"
no longer applies; "survives the plug being pulled" replaced it and is tested.

## What is in it

- **Video** (`main/vga.c`, `main/pie.S`): 640×480@60, RGB222. The RGB LCD driver runs in
  no-framebuffer mode and calls our fill routine for each 10-line bounce buffer. That routine
  copies from the PSRAM framebuffer (128-bit PIE loop when the buffer is aligned, `memcpy` otherwise), preloads the next lines into the
  cache, overlays the mouse pointer as a sprite, and records where the scan-out is.
- **Renderer and compositor** (`main/gfx.c`, `main/comp.c`): clipped primitives, two bitmap fonts,
  keyed blits. Damage is tracked as rectangles, painted strip by strip in a 20 KB internal-RAM
  buffer and copied to PSRAM once, timed to follow the beam (tear-free with one framebuffer).
- **Window manager and toolkit** (`main/wm.c`, `main/ui.c`, `main/menu.c`, `main/dlg.c`):
  overlapping windows, move/resize/minimise/maximise, modal dialogs, menu bars and popup menus,
  buttons, check boxes, radio buttons, text fields, sliders, list views with columns, drop-down lists, scrollbars,
  message boxes, a text prompt and an open/save dialog. Focus is always visible (dotted rectangle
  or highlight) and returns to the right place when a window closes.
- **Shell** (`main/shell.c`): desktop icons, taskbar with a button per window and an uptime clock,
  the Bevel menu, Alt+Tab switcher, shut-down dialog.
- **Input** (`main/input.c`, `main/usb_input.c`): USB host HID with a report-descriptor parser, so
  boot keyboards, relative mice and absolute pointers (the JetKVM) all work; key repeat, Caps Lock,
  US layout.
- **Apps** (built in): Files, Text Editor (gap buffer in PSRAM, selection, clipboard, find,
  files larger than the screen), Picture Viewer (BMP 1/4/8/24/32-bit, Floyd-Steinberg dithering
  to 64 colours, zoom and pan), Terminal (ls, cd, cat, rm, mkdir, cp, mv, mem, df, run, ...),
  Settings (desktop colour, pointer speed, key repeat; saved to the drive), System Information,
  Tasks, Help (every shortcut, F1), Benchmark, Widget Gallery.
- **Loadable apps** (`main/app_loader.c`, `apps/`): ELF files in `/disk/Apps`, loaded with
  Espressif's `elf_loader` into PSRAM and run in their own task. The interface is one header,
  `apps/bevel.h` (a table of 24 functions: window, events, drawing, files, time, memory, message
  box); `tools/build_apps.sh` builds them. Included: **Snake** (the game), **Hello** (sample) and
  **CrashTest** (misbehaves on purpose).
- **Doom** (`apps/doom/doom_bevel.c`, `tools/build_doom.sh`): the open-source Doom engine
  (doomgeneric, GPL-2) built as a 348 KB loadable program, `/disk/Apps/Doom.elf`, playing the
  shareware `doom1.wad` (4.2 MB, on the drive). It renders 320x200 in 256 colours; the platform
  layer shows it at 640x400 and uses the four screen pixels of each game pixel for a 2x2 ordered
  dither into the 64-colour palette. About 32 frames per second, keyboard controlled, no sound
  (the board has no audio output). To host it the loader gained a C-library symbol table whose
  memory and file calls are tracked per app, so closing or ending Doom returns its 5.5 MB.
- **USB hubs**: hub support is enabled in the USB host stack, so a keyboard and a mouse can be
  two devices behind a splitter. Not tested with a physical hub yet.
- **Robustness**: CPU exceptions inside an app task are caught and become an error dialog; an app
  that stops reading events is marked "not responding" and can be ended from the task list
  (Ctrl+Alt+Del); app memory is tracked and freed. Files are saved through a temporary file and a
  rename. The bundled programs and sample files are also stored in the firmware and are put back
  on the drive if it ever has to be reformatted. The shut-down screen restarts on any key, so
  the reset button is never needed.
- **Test channel** (`main/serial.c`, `tools/bv.py`): 2 Mbaud serial commands for framebuffer
  dumps (0.4 s per PNG), key and mouse injection, window/focus state, benchmarks and file checks.

## Measured (details in `docs/PERF.md`)

| | |
|---|---|
| Boot to desktop | 0.7 s |
| Mouse pointer | 60 FPS always, 0 framebuffer writes per move |
| Window drag, full contents, tear-free | 68 FPS (488×357 window) |
| Full-screen redraw, tear-free | 76 FPS |
| Scan-out cost | 18.0 µs per 10 lines, 5.6% of one core, 0 late fills (12.1 µs in builds where the bounce buffers are 16-byte aligned) |
| Free RAM at the desktop | 242 KB internal, 7.1 MB PSRAM |
| Firmware | 824 KB |

## Known limits

- The Doom data file `doom1.wad` is too large to keep a spare copy in the firmware. If the drive
  is ever reformatted, copy it back with `tools/bv.py put third_party/wad/doom1.wad /disk/Apps/doom1.wad`
  (`tools/fetch_doom.sh` downloads it). Doom's saved games go to `/disk/System`.
- While Doom runs, the worst-case scan-out slack drops from 186 µs to about 65-85 µs of 318 µs
  (still 0 late fills): Doom executes from PSRAM and keeps the bus busy.

- Loaded apps get a canvas, input events, files and a message box. They cannot create toolkit
  widgets (buttons, lists) of their own yet.
- An app that faults while it is inside an OS call (for example, passes a bad pointer to a file
  call) still takes the system down and it restarts; faults in the app's own code are caught.
- The text "viewer" is the editor; there is no separate read-only viewer.
- Tearing was checked by the scan-out counters (0 late fills, flush always behind the beam), not
  by eye: the JetKVM stream is recompressed video and cannot show a single torn frame.
- The picture is 640 pixels wide on a line timed for 678, so a capture device shows narrow black
  bars left and right; a VGA monitor's auto-adjust removes them.
- Cold power-up was tested with the reset line (same path for the firmware and the drive), not by
  physically pulling the plug.
- No real-time clock: the taskbar shows uptime.
- PSRAM runs at the stable 80 MHz; 120 MHz measured 58% faster but is experimental.

## Human interventions

See `INTERVENTIONS.md`: rows 0-26 are the hardware bring-up before the OS work. During the OS
session there were 3 entries (27: the JetKVM USB cabling that was already in place when the
session started; 28: the decision not to use an SD card; 29: approving the Doom downloads). No reset button presses, no rewiring,
no plugging was needed while the OS was built.

## How to work on it

```
tools/flash.sh                      build, flash, restart the serial daemon
tools/build_apps.sh                 rebuild apps/*.c into disk/Apps/*.elf
tools/bv.py dump out.png            pixel-exact screenshot
tools/bv.py cmd "key ctrl+esc"      inject input ("help" is in main/serial.c)
tools/kbtest.py                     keyboard-only acceptance test
tools/powercut.py 12                power-cut test
tools/bv.py cmd "perf all" 60       benchmarks
```
