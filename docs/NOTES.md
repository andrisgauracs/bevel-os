# Hardware & bring-up notes

- Board: ESP32-S3 (QFN56) rev v0.2, 16 MB flash, 8 MB octal PSRAM (N16R8). UART port shows as
  `/dev/cu.usbmodem<serial>` (WCH CH343). Opening the port does not always reset the board;
  `tools/serlog.py` pulses RTS to force a reset.
- Extra wire (added 2026-11-10, approved): devkit 5V → VGA pin 9.
- USB input (see docs/HANDOFF-AMENDMENT-USB.md): USB-C female breakout wired D−→GPIO19, D+→GPIO20,
  G→GND, V→5V. The board's own native USB connector must stay unplugged.
- The board's "5Vin" pin is input-only (diode): it measures 0.7 V when the board runs from USB, so
  the breakout's V pad (and VGA pin 9) get no 5 V from it. USB devices on the breakout stay unpowered
  until V gets an external 5 V supply. The board has an open "USB-OTG" solder jumper on the back.
  The breakout has no CC resistors, so a Mac USB-C port will not talk to it directly.
- Video: the LCD peripheral's fractional clock divider (needed for exactly 25.175 MHz) produces a
  signal the Acer test monitor refuses. Use integer-divided pixel clocks. Default is 640x480 at
  26.67 MHz (160 MHz / 6), H total 848, 31.45 kHz / 59.9 Hz, negative sync. 24 MHz (PLL240/10),
  800x600 @ 40 MHz and positive sync all also worked.
- First converter bought was HDMI→VGA (wrong direction). Replacement: VGA-source-to-HDMI with USB
  power; needs a VGA female-female coupler and an HDMI cable.
- JetKVM at <jetkvm-ip> (on the local network) (one viewer session at a time). Chrome needed macOS Local Network permission.
  Working chain (2026-11-12): box VGA (gender changer) → VGA-to-HDMI adapter WITH its USB power
  plugged in → HDMI → JetKVM. Default mode 1 (640x480) shows. Chrome may ask to start the stream
  manually ("Autoplay permissions required").
- Serial commands (firmware): D dump framebuffer, P test pattern, W/K/R/G/B solid fill, 0-4 test modes.
  Tools: tools/fbdump.py, tools/serbridge.py, tools/cycle_modes.py, tools/serlog.py.
- OS session (2026-11-12): the OS is "Bevel OS". No SD card (user decision): files live on a FAT
  partition in flash (`storage`, 12 MB, mounted at /disk; see partitions_esp32s3.csv).
- Console and command channel run at 2 Mbaud (CONFIG_ESP_CONSOLE_UART_CUSTOM). The old single-letter
  commands are gone except `D`; use `tools/bv.py` (daemon holds the port) and `tools/bvx.sh`.
- Video timing is still 26.67 MHz / H total 848, but the porches are now 36 front / 70 back so the
  picture is centred in a capture that expects standard timing. An absolute pointer (JetKVM) is
  mapped with that in mind (POINTER_SPAN_X / POINTER_OFFS_X in main/display.h).
- JetKVM USB: enumerates as keyboard + absolute pointer + relative mouse + a vendor interface.
  Browser automation does not send real Shift presses to it; use the JetKVM "Paste text" box to
  test capitals.
- The scan-out interrupt must stay on the same core as the GUI task (see docs/PERF.md).
- Doom: engine source and WAD are not in the repo (`tools/fetch_doom.sh`); `tools/build_doom.sh` builds
  `disk/Apps/Doom.elf`, which is embedded in the firmware. `tools/elf_fix.py` removes one empty relocation
  that the loader's validator rejects. Bundled programs on the drive are refreshed when the firmware's
  build stamp (`/disk/System/firmware.id`) changes.
- USB hubs: CONFIG_USB_HOST_HUBS_SUPPORTED=y. The hub and its devices draw power from the box's "5V" port.

