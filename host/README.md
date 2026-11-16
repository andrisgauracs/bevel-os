# Bevel OS desktop simulator

The OS of `main/` compiled for Linux (x86-64), shown in an SDL window. It is not an emulator: the
same C sources run natively, and only the bottom layer is replaced.

| On the board | In the simulator |
|---|---|
| FreeRTOS tasks on two cores | threads (`rtos.c`); queues and semaphores on POSIX semaphores |
| VGA scan-out (`main/vga.c`) | the headless backend (`main/headless.c`), drawn into the window 60 times a second (`sim.c`) |
| USB keyboard and mouse | SDL events; the characters typed with the host's layout (`--raw-keys`: physical keys, US layout, like a USB keyboard) |
| FAT drive in flash at `/disk` | a folder (`--disk`, default `build-host/disk`); file calls are wrapped at link time (`fswrap.c`) |
| Wi-Fi, esp_http_client | always connected; pages fetched with the `curl` command (`net_host.c`) |
| ELF apps, Xtensa exception table | the same `apps/*.c` built as shared objects (still named `.elf`), `dlopen`, POSIX signals (`app_dl.c`) |
| UART0 serial channel | TCP 127.0.0.1:47812 (`hal.c`), also copied to the terminal (`--quiet` to stop that) |
| VNC server on the board's address | the same `main/vnc.c`, on 127.0.0.1:5900 only (`-DBEVEL_VNC_PUBLIC=ON` for all interfaces) |

The ESP-IDF headers the OS includes are one-line files in `include/` that pull in
`include/host_port.h`, so no `#ifdef` is needed in the OS for the simulator, apart from three
newlib symbols in the app loader's import table.

## Build and run

Needs gcc, CMake, the SDL2 runtime library (any Linux desktop has it) and its headers.
Without `libsdl2-dev` and without root, `host/get-sdl2.sh` unpacks the Debian/Ubuntu headers into
`build-host/deps`.

```sh
cmake -S host -B build-host && cmake --build build-host -j
build-host/bevel-sim [--scale N] [--disk DIR] [--serial-port P] [--headless] [--raw-keys] [--quiet]
```

CMake options: `-DBEVEL_VNC=OFF`, `-DBEVEL_VNC_PORT=5901`, `-DBEVEL_VNC_PASSWORD=secret`,
`-DBEVEL_VNC_PUBLIC=ON`, `-DBEVEL_DOOM=OFF` (Doom is built from `third_party/doomgeneric`; put
`doom1.wad` in `build-host/disk/Apps/` to play it).

The bundled files (Welcome.txt, Bevel.bmp, the apps) are linked into the executable and written to
the drive by `main/storage.c`, exactly as the firmware does; a new build reinstalls the apps.

## Tests

```sh
build-host/bevel-sim --headless --quiet &
BV_PORT=tcp:127.0.0.1:47812 python3 tools/bv.py daemon &
python3 tools/kbtest.py       # keyboard-only acceptance test
python3 tools/vnctest.py      # VNC server test
python3 tools/bv.py dump screen.png
```

## Differences from the board

- Timing: nothing waits for a display beam, there is no PSRAM, and the CPU is much faster. The
  benchmark and the scan-out statistics mean nothing here; frame pacing and race conditions that
  depend on the ESP32-S3's speed can differ.
- Memory figures in System Information are the board's sizes, not real measurements.
- An app is ended with a signal (End task): if it is stopped while inside the C library (for
  example inside `malloc` from its own code), a lock may stay held. On the board the same risk is
  limited the same way, by not ending an app while it is inside an OS call.
- Keyboard: only ASCII characters reach the OS, which has a US layout (as on the board).
