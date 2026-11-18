# Bevel OS

![Bevel OS desktop: the System window, the keyboard shortcut list and the Alt+Tab switcher](docs/img/cover.png)

A small graphical desktop operating system for one ESP32-S3 board, drawing a 640x480 VGA picture
in 64 colours through six resistors. It has overlapping windows, a taskbar, a file manager, a text
editor, a picture viewer, a terminal, Wi-Fi with a text web browser, and apps loaded from its own
drive as separate ELF files, including Snake and Doom.

The same tree also builds for a second board: a Guition JC1060P470-V2 (ESP32-P4) with a 7-inch
MIPI-DSI panel, touch, wired Ethernet and a card reader. Pick it with `idf.py --preset esp32p4`;
[docs/PORT-JC1060P470-V2.md](docs/PORT-JC1060P470-V2.md) says what that port involved and what has
not been tested on that board yet.

The OS was written by an AI model, **Claude Fable 5.2**, working as a coding agent in Claude
Code. The hardware bring-up before it (colour test, USB input, the 3D-printed box, and the brief
for the OS) was also done with Claude in an earlier session. A human did the physical work
(wiring, plugging cables, 3D printing) and made a few decisions; every one of those actions is
logged in [INTERVENTIONS.md](INTERVENTIONS.md) and summarised [below](#what-the-human-did).

## Screenshots

<table>
<tr>
<td width="33%"><img src="docs/img/apps.png" alt="Files and the text editor"></td>
<td width="33%"><img src="docs/img/doom.png" alt="Doom running in a window"></td>
<td width="33%"><img src="docs/img/browser.png" alt="The web browser showing FrogFind search results"></td>
</tr>
<tr>
<td>Files and the Text Editor</td>
<td>Doom (shareware E1M1) as a loadable app</td>
<td>The browser: FrogFind search results over Wi-Fi</td>
</tr>
</table>

These are pixel-exact framebuffer dumps taken over the serial port (`tools/bv.py dump`), shown
at 2x, not photos. More in [captures/](captures/), all from the real board.

## Contents

- [Screenshots](#screenshots)
- [What it does](#what-it-does)
- [Hardware](#hardware)
- [How it works](#how-it-works)
- [Building and flashing](#building-and-flashing)
- [Apps and files](#apps-and-files)
- [Doom](#doom)
- [Limitations](#limitations)
- [What the human did](#what-the-human-did)
- [Repository layout](#repository-layout)
- [Licence](#licence)

## What it does

- Boots to the desktop in 0.7 s.
- Windows move, resize, minimise, maximise and close; menus, dialogs, Alt+Tab, a task list
  (Ctrl+Alt+Del). Everything works with the mouse alone and with the keyboard alone (F1 lists
  every shortcut).
- Built-in programs: Files, Text Editor, Picture Viewer (BMP, dithered to 64 colours), Terminal,
  Settings, System Information, Tasks, Help, Benchmark, Widget Gallery, Network (Wi-Fi) and
  Browser. On the P4 board also Camera (live picture, pictures saved as BMP) and Sound Player.
- Loadable programs: ELF files in `/disk/Apps`, run in their own task through a 24-function API
  (`apps/bevel.h`). Included: Snake (4.8 KB), Hello, CrashTest and Doom.
- A crash in an app's own code becomes an error dialog and the desktop keeps running; a hung app
  is marked "not responding" and can be ended.
- Remote screen: with Settings > Remote access switched on, `http://<board address>/` in a browser
  on another machine shows the screen and takes that browser's mouse, wheel and keyboard (keys go
  over as physical keys, so the board's keyboard layout applies). There is no password: anyone on
  the network can use it while it is on, so it is off until switched on. `main/remote.c`. Its
  Files button opens `http://<board address>/files`, which lists the drive, the SD card and a USB
  stick, downloads and uploads files, and renames, deletes and makes folders (`main/remote_files.c`).
- USB sticks: a FAT-formatted stick on the USB host port, directly or behind a hub, appears as
  `/disk/USB` in Files, the file dialogs, apps and the web file browser. Eject (the button inside
  the drive, File > Eject, the right-click menu or `eject` in the Terminal, with `usb` or `sd` when both are
  mounted) unmounts a stick or an SD card so it can be taken out safely.
- Original look in the mid-90s style: own bitmap fonts, icons and cursors (drawn as ASCII art in
  `tools/assets/src/`), no copied artwork.

Measured on the board (details in [docs/PERF.md](docs/PERF.md)):

| | |
|---|---|
| Boot to desktop | 0.7 s |
| Mouse pointer | 60 FPS, zero framebuffer writes per move |
| Window drag, full contents, tear-free | 68 FPS (488x357 window) |
| Full-screen redraw, tear-free | 76 FPS |
| Doom (E1M1) | about 32 FPS |
| Scan-out cost | 5.6% of one core, 0 late fills in every benchmark |
| Keyboard-only test (`tools/kbtest.py`) | 34 of 34 steps |
| Reset during a file write (`tools/powercut.py`) | 12 of 12 boots with the drive intact |

## Hardware

### Parts

- **ESP32-S3 board with 16 MB flash and 8 MB octal PSRAM (N16R8)**, chip revision v0.2. The board
  used has two USB-C ports: **COM** (a WCH CH343 USB-serial chip; used for flashing, logs, the
  serial tool and power) and **AUX** (the chip's native USB on GPIO19/20, which must stay
  unplugged because the firmware uses those pins as a USB host).
- **Resistors for the DAC:** three 1 kΩ, and three 440 Ω made from two 220 Ω in series each.
- **A VGA cable**, cut open, wired to the resistors and sync pins.
- **Two 4-pad USB-C female breakouts** (pads V, G, D−, D+): one for the keyboard/mouse, one as
  a 5 V power input.
- **A 5 V USB charger** for the power-input breakout.
- **A VGA monitor.** Optional: a VGA-to-HDMI adapter (one that takes a VGA *source*, with its own
  USB power) and a VGA female-female gender changer, to view the screen through HDMI capture
  or a network KVM. During development the screen was viewed through a JetKVM.
- **A USB keyboard and mouse**, or a combined keyboard-and-mouse receiver (see
  [Limitations](#limitations) about hubs).
- Optional: the 3D-printed box in [enclosure/](enclosure/) (Blender scripts and STLs).

<img src="enclosure/out/preview_exploded.png" width="480" alt="Render of the 3D-printed enclosure">

### VGA wiring (resistor DAC)

Each colour channel uses two GPIOs joined after their resistors, a 2-bit DAC into the monitor's
75 Ω input: about 0, 0.2, 0.45 and 0.65 V. Three channels of two bits give 64 colours (RGB222).

| ESP32-S3 GPIO | Signal | Resistor | VGA pin |
|---|---|---|---|
| 4 | Red, high bit | 440 Ω (2 x 220 Ω) | 1 |
| 5 | Red, low bit | 1 kΩ | 1 |
| 6 | Green, high bit | 440 Ω | 2 |
| 7 | Green, low bit | 1 kΩ | 2 |
| 15 | Blue, high bit | 440 Ω | 3 |
| 16 | Blue, low bit | 1 kΩ | 3 |
| 17 | H-sync | none | 13 |
| 18 | V-sync | none | 14 |
| GND | Ground | | 5, 10, cable braid |

The pin numbers are in `main/vga.c`. The LCD peripheral's 8-bit bus maps D0-D5 to GPIO 5, 4, 7,
6, 16, 15, so one byte is one pixel: `(B << 4) | (G << 2) | R`, each 0-3.

**Video timing** (`main/vga.c`): 640x480 at 59.9 Hz, pixel clock 26.67 MHz (160 MHz / 6),
horizontal 640 + 36 front porch + 102 sync + 70 back porch = 848, vertical 480 + 10 + 2 + 33 =
525, both syncs negative; 31.45 kHz line rate. The standard 25.175 MHz clock needs the LCD
peripheral's fractional divider, and the test monitor refused that signal, so only
integer-divided clocks are used.

The board in this build also has a wire from its 5V pin to VGA pin 9, but that pin of the board
is input-only and carries no 5 V, so a VGA-to-HDMI adapter needs its own USB power.

### USB input wiring

| USB-C breakout pad | Connected to |
|---|---|
| D− | GPIO 19 |
| D+ | GPIO 20 |
| G | GND |
| V | the board's 5V pin |

The board's 5V pin is input-only (it sits behind a diode and measured 0.7 V when the board ran
from USB), so on its own it gives the breakout no power. A second breakout is wired as a power
input (V to the same 5V node, G to GND) and a USB charger is plugged into it. The breakouts have
no CC resistors, so a USB-C-to-USB-C cable from a computer will not talk to them.

More detail: [docs/HANDOFF.md](docs/HANDOFF.md), [docs/HANDOFF-AMENDMENT-USB.md](docs/HANDOFF-AMENDMENT-USB.md),
[docs/NOTES.md](docs/NOTES.md), [enclosure/README.md](enclosure/README.md).

## How it works

Bevel OS runs on ESP-IDF 5.5.3 and FreeRTOS, which provide the boot chain, USB host stack, FAT,
Wi-Fi/TLS and the scheduler. Everything visible is written from scratch (about 11 000 lines of
C and a little assembly). The full description is in [docs/OS-REPORT.md](docs/OS-REPORT.md).

- **VGA scan-out** (`main/vga.c`, `main/pie.S`): the RGB LCD peripheral runs in no-framebuffer
  mode with two 10-line bounce buffers in internal RAM. Its interrupt calls a fill routine that
  copies the next lines from the 300 KB framebuffer in PSRAM (a 128-bit SIMD loop when the buffer
  is aligned), preloads the following lines into the cache, draws the mouse pointer on top as a
  sprite, and records where the beam is. The pointer never touches the framebuffer.
- **Compositor** (`main/comp.c`, `main/gfx.c`): nothing draws straight into PSRAM, which is the
  bottleneck (about 35 MB/s writes, shared with the display). Damage is tracked as rectangles,
  painted strip by strip in a 20 KB internal-RAM buffer, then copied to PSRAM once, just behind
  the beam. That gives tear-free updates with a single framebuffer; it also turned out cheaper
  than not waiting, because the scan-out has just pulled those rows into the cache.
- **Window manager and toolkit** (`main/wm.c`, `main/ui.c`, `main/menu.c`, `main/dlg.c`,
  `main/shell.c`): z-order, focus, keyboard navigation, buttons, lists, drop-down lists, text fields, sliders,
  menus, dialogs, the taskbar and desktop. No LVGL.
- **Cores:** the scan-out interrupt and the GUI task share core 1 (moving the interrupt to the
  other core broke beam racing; see [docs/PERF.md](docs/PERF.md)). USB, the drive, the serial
  channel and loaded apps run on core 0.
- **Input** (`main/usb_input.c`, `main/input.c`): USB host HID on GPIO19/20 with a report
  descriptor parser, so boot keyboards, relative mice and absolute pointers all work, directly or
  behind a hub. A watchdog revives the port after a failed enumeration. The S3's controller has 8
  host channels, so devices the OS cannot use (a dock's billboard device or network adapter) are
  turned away during enumeration.
- **USB sticks** (`main/usb_storage.c`): Espressif's `usb_host_msc` driver, mounted at `/disk/USB`.
  Input comes first: a stick is claimed once the bus is quiet, is left unmounted if too few host
  channels remain, and gives its channels back if a keyboard or mouse needs them later.
- **Storage** (`main/storage.c`, `partitions.csv`): the S3 board has **no SD card**. The drive is a
  12 MB wear-levelled FAT partition in the board's own flash, mounted at `/disk`. It is formatted
  automatically if it cannot be mounted. The bundled apps and sample files are also embedded in the
  firmware and are written back to the drive when they are missing or when the firmware changes
  (`/disk/System/firmware.id`). Settings live in `/disk/System/settings.ini`.
- **Apps** (`main/app_loader.c`, `apps/`): position-independent ELF files loaded with Espressif's
  `elf_loader` into PSRAM, each in its own task with a private canvas. Apps see only the function
  table in `apps/bevel.h` plus a small C-library table; their memory and open files are tracked
  and released when they exit or are ended.
- **Wi-Fi and browser** (`main/net.c`, `main/app_net.c`, `main/app_browser.c`): the radio starts
  only when the Network panel or browser is opened or a network is saved. The browser does HTTP and
  HTTPS GET, follows redirects, and lays out HTML as text: headings, lists, links, preformatted
  text and simple search forms, pages up to 2 MB. No images, scripts or style sheets.
- **Serial channel** (`main/serial.c`, `tools/bv.py`): a command line on the COM port (USB on the P4, 2 Mbaud on the S3)
  for framebuffer dumps (a PNG in 0.4 s), key and mouse injection, benchmarks and file upload.
  This is how the agent saw and tested the screen.

### Flash layout (`partitions_esp32s3.csv`, or `partitions_esp32p4.csv` on the P4 board)

| Name | Type | Offset | Size (S3 / P4) |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB |
| phy_init | data/phy | 0xF000 | 4 KB |
| factory | app | 0x10000 | 3 MB / 4 MB |
| storage | data/fat | 0x310000 / 0x410000 | 12 MB / just under 12 MB |

The P4 board gets the bigger app partition because its build carries the camera app; its firmware
also runs Wi-Fi through the on-board ESP32-C6, which needs room in the bootloader config.

## Building and flashing

Requirements: [ESP-IDF **v6.1**](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/), the
version both boards are built with here (the S3 machine was developed under v5.5). One tree, two
targets:

```sh
git clone https://github.com/andrisgauracs/bevel-os
cd bevel-os
. $HOME/esp/esp-idf/export.sh          # or wherever ESP-IDF v6.1 is installed
idf.py --preset esp32s3 build          # or --preset esp32p4; fetches elf_loader etc. the first time
idf.py --preset esp32s3 -p PORT flash  # PORT = the board's COM port
```

The two boards are the two configure presets in `CMakePresets.json`. Each builds in its own
directory (`build-esp32s3/`, `build-esp32p4/`) with its own `sdkconfig` there, so switching boards
rebuilds nothing of the other one. ESP-IDF 6.1's `idf.py` reads the presets file: without
`--preset` (or `IDF_PRESET` in the environment) it takes the first one, the S3. In VS Code, the
ESP-IDF extension's "Select Project Configuration" offers the same two.

On Windows, `tools\flash.ps1 -Target esp32p4 -Port COM13` does build, flash and the serial daemon in
one go (the P4 is its default). The S3 board's console is UART0 at 2 Mbaud; the P4 board talks over
its USB-Serial-JTAG port. An S3 devkit whose USB-UART bridge is a CH340 rather than a CP210x loses
bytes at 2 Mbaud: build it with the third preset, `esp32s3-ch340`, which runs the console at 460800
(`tools\flash.ps1 -Target esp32s3-ch340 -Port COM5`); `tools/bv.py` sees the CH340 and follows.

`sdkconfig.defaults` holds what both boards share, and `sdkconfig.defaults.esp32s3` /
`sdkconfig.defaults.esp32p4` add what differs (PSRAM speed, the console, the partition table, Wi-Fi
through the C6 on the P4); a preset's `sdkconfig` is generated from those two files the first time
it is configured, and none is kept in the tree. To change an option for good, put it in one of the
defaults files; `idf.py --preset <board> menuconfig` only changes that build directory's copy.
The `storage` partition is not flashed: the firmware formats it on first boot and fills it with the
bundled files.

The prebuilt apps in `disk/Apps/esp32s3/` and `disk/Apps/esp32p4/` are embedded into the firmware, so
the build works without building them first. `tools/build_apps.sh esp32s3|esp32p4` and
`tools/build_doom.sh esp32s3|esp32p4` rebuild them for the chosen chip.

### The serial tool

Opening the COM port can reset the board, so `tools/bv.py` runs a small daemon that holds the
port, logs to `build/serial.log` and serves commands on localhost. It needs `pyserial` (the
ESP-IDF Python environment has it). The port comes from `BV_PORT`, otherwise the first
`/dev/cu.usbmodem*`, `/dev/ttyACM*` or `/dev/ttyUSB*` is used.

```sh
python3 tools/bv.py daemon &                         # hold the port
python3 tools/bv.py dump screen.png                  # pixel-exact screenshot
python3 tools/bv.py cmd "key ctrl+esc"               # inject a key (commands: handle_line() in main/serial.c)
python3 tools/bv.py put local.txt /disk/Documents/local.txt   # copy a file to the drive
python3 tools/bv.py quit                             # release the port before flashing
```

A session that may not start a background process or listen on a port (a sandboxed one, for example)
can use `direct`, `directdump` and `directput` instead: the same three jobs, with the port opened and
closed by each call instead of by the daemon. Only one of the two may hold the port at a time.

`tools/flash.sh`, `tools/build.sh`, `tools/fetch_doom.sh` and the test scripts came from the Mac the
S3 machine was developed on (zsh, ESP-IDF sourced from `~/esp/esp-idf`, BSD `stat -f`); on Linux,
adjust those. On Windows the build-flash-monitor loop is `tools\flash.ps1`, and the two app build
scripts run under bash in a shell where ESP-IDF is active.

## Apps and files

An app is one C file that includes `apps/bevel.h` and exports
`int app_main(const bevel_api_t *os, const char *arg)`. See `apps/Hello.c` and `apps/Snake.c`.

```sh
tools/build_apps.sh esp32s3               # apps/*.c -> disk/Apps/esp32s3/*.elf (or: esp32p4)
python3 tools/bv.py put disk/Apps/esp32s3/Snake.elf /disk/Apps/Snake.elf
```

Or rebuild and flash the firmware, which embeds the four programs of whichever board you are
building and writes them to the drive on the next boot. Run an app from Files, the Bevel menu
(Programs), or the Terminal (`run Snake`).

## Doom

Doom is a loadable app built from [doomgeneric](https://github.com/ozkl/doomgeneric) (GPL v2,
included in `third_party/doomgeneric/`) and the platform layer `apps/doom/doom_bevel.c`. It
renders 320x200 in 256 colours and is shown at 640x400. On the P4 board the game's own palette goes
straight into the window's true-colour layer and the sound effects go to the OS mixer; both have
been seen and heard working there. The S3 board has no true-colour layer, so there the four screen
pixels of each game pixel carry a 2x2 ordered dither into the 64-colour palette, and there is no
sound.

`Doom.elf` is already in `disk/Apps/esp32s3/` and `disk/Apps/esp32p4/`, and in both firmwares. The
game data is not in this repository. `tools/fetch_doom.sh` downloads the shareware `doom1.wad` v1.9
from a third-party GitHub mirror,
[github.com/Akbar30Bill/DOOM_wads](https://github.com/Akbar30Bill/DOOM_wads), and checks its MD5
(`f0cefca49926d00903cf57551d901abe`). Any shareware v1.9 `doom1.wad` with that checksum works;
put it at `third_party/wad/doom1.wad` and the script skips the download. Then copy it to the
drive (4.2 MB, about two minutes over the serial port):

```sh
tools/fetch_doom.sh                                  # downloads third_party/wad/doom1.wad, checks the MD5
python3 tools/bv.py put third_party/wad/doom1.wad /disk/Apps/doom1.wad
```

To rebuild `Doom.elf` from source: `tools/build_doom.sh esp32s3` or `tools/build_doom.sh esp32p4`.

## Limitations

- **USB host channels are scarce on the S3.** It has 8: every device takes one, a hub a second,
  each keyboard or mouse interface one more, and a stick two more. A plain hub with a
  keyboard-and-mouse receiver and a stick uses all 8; behind a dock with two internal hubs, the
  stick stays unmounted. The P4 has 16.
- **USB is full speed on both boards** (about 500 KB/s reading and 800 KB/s writing measured with a
  1 GB stick on the S3). **Sticks must be FAT formatted**, exFAT and NTFS are not supported, and only
  one is used at a time.
- **The browser is text only:** no images, JavaScript, style sheets or form POSTs. Google search
  needs JavaScript and fails; typing words in the address bar searches FrogFind instead.
- **The Wi-Fi password is stored in plain text** on the drive (`/disk/System/wifi.ini`).
- **Loadable apps** get a canvas, events, files and a message box, but cannot create toolkit
  widgets. An app that faults inside an OS call (for example, a bad pointer passed to a file call)
  still restarts the system; faults in the app's own code are caught.
- **doom1.wad is not restored** if the drive is reformatted (it is too big to keep a copy in the
  firmware). Doom's saved games were not tested. Doom and the browser together were not tested;
  with Wi-Fi on, free internal RAM drops from about 240 KB to about 125 KB.
- **Tearing** was checked with the scan-out counters, not by eye.
- **No real-time clock:** the taskbar shows uptime.
- **The picture is 640 pixels on a line timed for 678**, so capture devices show narrow black
  bars at the sides; a VGA monitor's auto-adjust removes them.
- **Power loss:** the 12-of-12 test used resets during a file write. A real unplug was tested once
  (the OS came back, but the JetKVM's USB input did not until a port watchdog was added; that fix
  was not re-tested with the JetKVM).
- PSRAM runs at 80 MHz. 120 MHz measured about 58% faster but is experimental in ESP-IDF, so it
  is off.

## What the human did

The full log, with times and reasons, is [INTERVENTIONS.md](INTERVENTIONS.md) (rows 0-38).
In short:

- **Before the software started (row 0):** wired the resistor DAC, the VGA cable and the converter.
- **Hardware bring-up (rows 1-26):**
  - plugged and moved cables;
  - three rewirings, each approved first: 5 V to VGA pin 9, the USB-C breakout on GPIO19/20, and
    the second breakout as a 5 V input;
  - multimeter checks;
  - described what the VGA monitor showed, before the agent could see the screen through a
    JetKVM;
  - allowed Chrome in a macOS privacy setting and typed a keyboard test;
  - bought a replacement for a VGA converter that turned out to be the wrong direction;
  - 3D-printed and fitted the enclosure.
- **OS session (rows 27-38):**
  - the JetKVM's USB cabling was already in place;
  - decided not to use an SD card (so storage moved to internal flash);
  - approved downloading doomgeneric and the shareware WAD;
  - did an unplug/replug power test;
  - tried USB splitters and dongles to get two input devices working;
  - typed the Wi-Fi password on the board's keyboard and tried the browser.

While the OS itself was built (renderer through Snake, tests and benchmarks), the human pressed
no reset button, did no rewiring and plugged nothing in.

How the work went, including what failed and what the agent got wrong:
[docs/BRINGUP-SESSION-SUMMARY.md](docs/BRINGUP-SESSION-SUMMARY.md) (hardware bring-up and
enclosure) and [docs/SESSION-SUMMARY.md](docs/SESSION-SUMMARY.md) (the OS). The brief the agent
was given is [docs/OS-BUILD-PROMPT.md](docs/OS-BUILD-PROMPT.md).

## Repository layout

```
main/            the OS: video, compositor, window manager, toolkit, shell, apps, storage,
                 USB input, Wi-Fi, browser, serial channel. board.h says which board is being built;
                 display_dsi.c is the P4 panel, display_vga.c plus vga.c the S3 scan-out
main/assets/     fonts, icons and cursors as C arrays (generated by tools/assets/gen_assets.py)
apps/            loadable apps and the app API header (bevel.h); apps/doom/ is the Doom port
disk/            files the firmware embeds and writes to the drive (Apps/esp32s3/ and Apps/esp32p4/
                 hold the prebuilt ELF programs, plus Welcome.txt, Bevel.bmp and the chime)
tools/           bv.py serial tool, build/flash scripts (they take a target), tests (kbtest.py,
                 powercut.py), asset sources
third_party/     doomgeneric (GPL v2); the Doom WAD is fetched, not committed
enclosure/       Blender scripts, STLs and renders for the 3D-printed box
captures/        framebuffer screenshots from the real board
docs/            design notes, performance log, hardware notes, the build brief, session summaries,
                 and the port status for the P4 board
INTERVENTIONS.md every manual action by the human
partitions_esp32s3.csv, partitions_esp32p4.csv   flash layout, one per board
sdkconfig.defaults and sdkconfig.defaults.esp32s3 / .esp32p4   the configuration both boards share
                 and what each adds; each preset's sdkconfig is generated in its build directory
CMakePresets.json   one configure preset per board, for idf.py --preset and the VS Code extension
```

Deep dives: [docs/OS-REPORT.md](docs/OS-REPORT.md) (what was built),
[docs/PERF.md](docs/PERF.md) (every measurement and the experiments behind each decision),
[docs/NOTES.md](docs/NOTES.md) (hardware and bring-up notes).

## Licence

Bevel OS is released under the [MIT licence](LICENSE).

Doom is the exception: `third_party/doomgeneric/` is GPL v2, so `apps/doom/` and the built
`disk/Apps/esp32s3/Doom.elf` and `disk/Apps/esp32p4/Doom.elf` are distributed under the GPL v2 as
well. `Doom.elf` is a separate program that the OS loads from the drive; the firmware also carries a
copy of it so it can be restored to the drive. If you distribute firmware images that include
`Doom.elf`, the GPL's source requirement applies to that part, and all of that source is in this
repository. To build without Doom, remove the `"${APP_DIR}/Doom.elf"` line from the `EMBEDDED` list
in `main/CMakeLists.txt` and the `BUNDLED(Doom, ...)` line in `main/storage.c`.

The Doom shareware data file (`doom1.wad`, id Software) is not included. The ESP-IDF components
`elf_loader`, `usb_host_hid` and `usb_host_msc` (Apache-2.0) are downloaded by the component manager at build time.
