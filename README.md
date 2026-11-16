# Bevel OS

![Bevel OS desktop: the System window, the keyboard shortcut list and the Alt+Tab switcher](docs/img/cover.png)

A small graphical desktop operating system for one ESP32-S3 board, drawing a 640x480 VGA picture
in 64 colours through six resistors. It has overlapping windows, a taskbar, a file manager, a text
editor, a picture viewer, a terminal, Wi-Fi with a text web browser, and apps loaded from its own
drive as separate ELF files, including Snake and Doom.

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
- [Remote desktop (VNC), simulator and emulator](#remote-desktop-vnc-simulator-and-emulator)
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
  Browser.
- Loadable programs: ELF files in `/disk/Apps`, run in their own task through a 24-function API
  (`apps/bevel.h`). Included: Snake (4.8 KB), Hello, CrashTest and Doom.
- A crash in an app's own code becomes an error dialog and the desktop keeps running; a hung app
  is marked "not responding" and can be ended.
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
  `main/shell.c`): z-order, focus, keyboard navigation, buttons, lists, text fields, sliders,
  menus, dialogs, the taskbar and desktop. No LVGL.
- **Cores:** the scan-out interrupt and the GUI task share core 1 (moving the interrupt to the
  other core broke beam racing; see [docs/PERF.md](docs/PERF.md)). USB, the drive, the serial
  channel and loaded apps run on core 0.
- **Input** (`main/usb_input.c`, `main/input.c`): USB host HID on GPIO19/20 with a report
  descriptor parser, so boot keyboards, relative mice and absolute pointers all work. Hub support
  is enabled in the USB host stack, and a watchdog revives the port after a failed enumeration.
- **Storage** (`main/storage.c`, `partitions.csv`): there is **no SD card**. The drive is a
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
- **Serial channel** (`main/serial.c`, `tools/bv.py`): a 2 Mbaud command line on the COM port
  for framebuffer dumps (a PNG in 0.4 s), key and mouse injection, benchmarks and file upload.
  This is how the agent saw and tested the screen.

### Flash layout (`partitions.csv`)

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB |
| phy_init | data/phy | 0xF000 | 4 KB |
| factory | app | 0x10000 | 3 MB |
| storage | data/fat | 0x310000 | 12 MB |

## Building and flashing

Requirements: [ESP-IDF **v5.5.3**](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/)
(the version in `dependencies.lock`), target `esp32s3`. The checked-in `sdkconfig` and
`sdkconfig.defaults` already select the target, 16 MB flash, octal PSRAM, the custom partition
table and the 2 Mbaud console.

```sh
git clone https://github.com/andrisgauracs/bevel-os
cd bevel-os
. $HOME/esp/esp-idf/export.sh          # or wherever ESP-IDF v5.5.3 is installed
idf.py build                            # downloads elf_loader and usb_host_hid on the first run
idf.py -p PORT -b 921600 flash          # PORT = the board's COM port, e.g. /dev/cu.usbmodem... or /dev/ttyACM0
```

Do not run `idf.py set-target`; it would regenerate `sdkconfig`. `idf.py flash` writes the
bootloader, the partition table and the app. The `storage` partition is not flashed: the
firmware formats it on first boot and fills it with the bundled files.

The prebuilt apps in `disk/Apps/*.elf` are embedded into the firmware, so the build works without
building them first.

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

The helper scripts (`tools/flash.sh`, `tools/build_apps.sh`, `tools/build_doom.sh`, ...) are zsh
scripts written for the development Mac: they source ESP-IDF from `~/esp/esp-idf` and use the
BSD `stat -f`. On Linux, adjust those two things.

## Remote desktop (VNC), simulator and emulator

Three ways to use Bevel OS without the VGA monitor. They share one display interface
(`main/vga.h`): the VGA scan-out (`vga.c`) is one backend, `headless.c` (framebuffer only) the
other, and the compositor reports every rectangle it redraws to the VNC server.

### VNC server

`idf.py menuconfig` → **Bevel OS**:

| Option | Effect |
|---|---|
| Local display: VGA / None | *None* never starts the LCD peripheral: the VGA pins are free and the scan-out costs nothing |
| VNC server | serves the desktop on TCP 5900, with or without VGA (both at once works as a built-in KVM) |
| VNC password | classic VNC authentication; empty = none. It is weak (DES, no encryption): local network only |
| Default Wi-Fi network | joined at boot when nothing was saved from the Network panel, so a headless board is reachable |

Any VNC viewer works (TigerVNC, RealVNC, Remmina, gvncviewer; noVNC through websockify). The
server speaks RFB 3.3/3.7/3.8 with Raw and Hextile, sends only what changed, and passes the
pointer as a cursor shape when the viewer supports it (moving the mouse then costs nothing on
the wire). The viewer's keyboard layout is respected: the characters typed are sent, not the key
positions, so an AZERTY keyboard types what it shows. One viewer at a time; a new one takes over.
System Information and the serial command `vnc` show the address and the connected viewer.

### Desktop simulator (Linux)

The same OS sources built for the PC, in an SDL window: FreeRTOS tasks run as threads, the drive
is a folder, the network is the host's (pages are fetched with `curl`), and the apps (Snake,
Doom...) are rebuilt as x86-64 shared objects loaded like the ELF files on the board, crash
handling and "End task" included. Useful to work on the UI without flashing, with a debugger or
sanitizers.

```sh
host/get-sdl2.sh                       # only if libsdl2-dev is not installed (no root needed)
cmake -S host -B build-host && cmake --build build-host -j
build-host/bevel-sim                   # --scale N, --disk DIR, --headless, --raw-keys
```

The drive is `build-host/disk` (put `doom1.wad` in `build-host/disk/Apps/` for Doom). The VNC
server listens on 127.0.0.1:5900 and the serial channel on 127.0.0.1:47812, so every tool works:
`BV_PORT=tcp:127.0.0.1:47812 python3 tools/bv.py daemon &`, then `kbtest.py`, `vnctest.py`,
`bv.py dump`. See [host/README.md](host/README.md).

### QEMU

The real firmware, Xtensa apps and all, in Espressif's QEMU (`machine esp32s3`). QEMU emulates
neither the LCD peripheral nor USB nor the Wi-Fi radio, so `sdkconfig.qemu` builds it headless
(no VGA), with VNC over the emulated OpenCores Ethernet, without USB input, without the PIE vector
instructions and with the flash in DIO mode.

**Setup (once).** ESP-IDF v5.5.3 (see [Building and flashing](#building-and-flashing)), then QEMU:

```sh
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa
```

QEMU needs `libslirp` for its network. If the system does not have it (`libslirp0` on
Debian/Ubuntu) and there is no root access, unpack it where `tools/qemu.sh` looks for it:

```sh
mkdir -p ~/esp/qemu-libs && cd ~/esp/qemu-libs
apt-get download libslirp0 && dpkg-deb -x libslirp0_*.deb root
```

**1. Start Bevel OS in QEMU** (from the repository):

```sh
tools/qemu.sh
```

It builds the firmware into `build-qemu/` with `sdkconfig.defaults` + `sdkconfig.qemu`, writes a
flash image and boots it. When it prints

```
Bevel OS in QEMU: VNC 127.0.0.1:5900, serial channel tcp:127.0.0.1:5555 (Ctrl+C to stop)
```

the desktop is up a few seconds later (about ten more on the very first run, while the bundled
files are written to the blank drive).

| Option | Effect |
|---|---|
| `--no-build` | boot the last build without rebuilding |
| `--wipe` | start from a blank drive |
| `--vnc-port P` | forward the VNC server to another host port (if 5900 is taken) |
| `--serial-port P` | serial channel on another host port (default 5555) |

**2. Connect with a VNC viewer** (another terminal):

```sh
gvncviewer 127.0.0.1:0                 # display :0 = port 5900
vncviewer 127.0.0.1:5900               # TigerVNC; Remmina and others work the same way
```

There is no password in the QEMU profile (set `CONFIG_BEVEL_VNC_PASSWORD` in `sdkconfig.qemu`
for one). The viewer's keyboard and mouse drive the OS. Both ports are forwarded on 127.0.0.1
only.

**3. Stop** with Ctrl+C in the QEMU terminal. The drive is kept between runs in
`build-qemu/qemu_flash.bin`.

**Serial channel (optional)**, for screenshots, commands, tests and copying files:

```sh
BV_PORT=tcp:127.0.0.1:5555 python3 tools/bv.py daemon &
python3 tools/bv.py cmd vnc                                   # VNC server state
python3 tools/bv.py dump screen.png                           # pixel-exact screenshot
python3 tools/bv.py put third_party/wad/doom1.wad /disk/Apps/doom1.wad   # for Doom (slow: ~10 min)
```

The emulator runs slower than the board; the scan-out and benchmark figures mean nothing there.

### Tests

`tools/vnctest.py` is a VNC viewer written for testing: it compares the picture it receives with
the framebuffer read over the serial channel, pixel by pixel (Raw 8-bit and Hextile 32-bit), types
Shift-dependent text into the editor, double-clicks and drags with the mouse, checks the pointer
reports, the password (with `openssl` as an independent DES) and the takeover by a second viewer.

| Test | Simulator | QEMU |
|---|---|---|
| `kbtest.py` (keyboard only, 34 steps) | 34 of 34 | 34 of 34 |
| `vnctest.py` (17 checks, 18 with `--password`) | 17 of 17, 18 of 18 | 17 of 17 |
| CrashTest: bad write, divide by zero, hang + End task | caught | caught |
| Doom E1M1 (the WAD sent with `bv.py put`, checksum verified) | runs | runs |

On the board, the VGA build, the VGA + VNC build and the headless + VNC build compile, but none
of this has run on the hardware yet: VNC over Wi-Fi (frame rate, CPU cost on core 0, internal
RAM) is unmeasured.

QEMU's emulated UART sometimes drops or repeats input under heavy traffic, so `bv.py put` sends
`putat <offset> <data>` lines that can be repeated safely, checks each acknowledgement's checksum,
retries, and verifies the size and an Adler-32 of the whole file at the end (`size`, `sum`).
Other serial commands added: `run <path>` (start a program from the drive), `vnc` (server state).

## Apps and files

An app is one C file that includes `apps/bevel.h` and exports
`int app_main(const bevel_api_t *os, const char *arg)`. See `apps/Hello.c` and `apps/Snake.c`.

```sh
tools/build_apps.sh                                  # apps/*.c -> disk/Apps/*.elf
python3 tools/bv.py put disk/Apps/Snake.elf /disk/Apps/Snake.elf
```

Or rebuild and flash the firmware, which embeds `disk/Apps/*.elf` and writes them to the drive on
the next boot. Run an app from Files, the Bevel menu (Programs), or the Terminal (`run Snake`).

## Doom

Doom is a loadable app built from [doomgeneric](https://github.com/ozkl/doomgeneric) (GPL v2,
included in `third_party/doomgeneric/`) and the platform layer `apps/doom/doom_bevel.c`. It
renders 320x200 in 256 colours and is shown at 640x400, using each game pixel's four screen
pixels for a 2x2 dither into the 64-colour palette. Keyboard only, no sound (the board has no
audio output).

`Doom.elf` is already in `disk/Apps/` and in the firmware. The game data is not in this
repository. `tools/fetch_doom.sh` downloads the shareware `doom1.wad` v1.9 from a third-party
GitHub mirror,
[github.com/Akbar30Bill/DOOM_wads](https://github.com/Akbar30Bill/DOOM_wads), and checks its MD5
(`f0cefca49926d00903cf57551d901abe`). Any shareware v1.9 `doom1.wad` with that checksum works;
put it at `third_party/wad/doom1.wad` and the script skips the download. Then copy it to the
drive (4.2 MB, about two minutes over the serial port):

```sh
tools/fetch_doom.sh                                  # downloads third_party/wad/doom1.wad, checks the MD5
python3 tools/bv.py put third_party/wad/doom1.wad /disk/Apps/doom1.wad
```

To rebuild `Doom.elf` from source: `tools/build_doom.sh`.

## Limitations

- **Two USB devices at once has not worked.** A single keyboard, a single mouse, or a combined
  keyboard-and-mouse receiver works. Hub support is enabled but was never seen working: the
  "splitter" tried was a Y-cable, not a hub, and two USB-C multiport dongles failed (one is not
  detected at all, the other fails the first USB request).
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
- **VNC:** one viewer at a time, and a new connection takes over before it has authenticated
  (so that a viewer whose connection died can always get back in): someone on the network can
  disconnect the current viewer, without seeing or controlling anything. Classic VNC
  authentication only, no encryption. No clipboard, no colour-map pixel formats, no ZRLE/Tight.
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
                 USB input, Wi-Fi, browser, serial channel
main/assets/     fonts, icons and cursors as C arrays (generated by tools/assets/gen_assets.py)
apps/            loadable apps and the app API header (bevel.h); apps/doom/ is the Doom port
disk/            files the firmware embeds and writes to the drive (Apps/*.elf, Welcome.txt, Bevel.bmp)
tools/           bv.py serial tool, build/flash scripts, qemu.sh, tests (kbtest.py, vnctest.py,
                 powercut.py), asset sources
host/            the desktop simulator: ESP-IDF/FreeRTOS on POSIX, SDL window, x86-64 app loader
third_party/     doomgeneric (GPL v2); the Doom WAD is fetched, not committed
enclosure/       Blender scripts, STLs and renders for the 3D-printed box
captures/        framebuffer screenshots from the real board
docs/            design notes, performance log, hardware notes, the build brief, session summaries
INTERVENTIONS.md every manual action by the human
partitions.csv   flash layout
sdkconfig.defaults, sdkconfig   ESP-IDF configuration; sdkconfig.qemu for the emulator
```

Deep dives: [docs/OS-REPORT.md](docs/OS-REPORT.md) (what was built),
[docs/PERF.md](docs/PERF.md) (every measurement and the experiments behind each decision),
[docs/NOTES.md](docs/NOTES.md) (hardware and bring-up notes).

## Licence

Bevel OS is released under the [MIT licence](LICENSE).

Doom is the exception: `third_party/doomgeneric/` is GPL v2, so `apps/doom/` and the built
`disk/Apps/Doom.elf` are distributed under the GPL v2 as well. `Doom.elf` is a separate program
that the OS loads from the drive; the firmware also carries a copy of it so it can be restored to
the drive. If you distribute firmware images that include `Doom.elf`, the GPL's source
requirement applies to that part, and all of that source is in this repository. To build without
Doom, remove `Doom.elf` from `EMBED_FILES` in `main/CMakeLists.txt` and the `BUNDLED(Doom, ...)`
line in `main/storage.c`.

The Doom shareware data file (`doom1.wad`, id Software) is not included. The ESP-IDF components
`elf_loader` and `usb_host_hid` (Apache-2.0) are downloaded by the component manager at build time.
