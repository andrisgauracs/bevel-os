// ESP32-S3 devkit (QFN56 rev v0.2, 16 MB flash, 8 MB octal PSRAM) as it is wired: a resistor DAC
// into a VGA monitor, keyboard and mouse on a USB-C breakout on the native USB pins. The hardware
// notes, including the two approved wiring changes, are in docs/NOTES.md and docs/HANDOFF-AMENDMENT-USB.md.
//
// Do not change the VGA pins without asking the human: the DAC is built around these ones.
#pragma once

#define BOARD_NAME            "ESP32-S3 VGA devkit"
#define BOARD_CHIP            "ESP32-S3"

// What this board has wired up. The OS skips what is missing instead of failing.
#define BOARD_HAS_TOUCH       0
#define BOARD_HAS_RTC         0       // no clock chip: the time comes from the network only
#define BOARD_HAS_I2C         0
#define BOARD_HAS_AUDIO       0       // no codec, no amplifier, no speaker
#define BOARD_HAS_SD          0       // no card reader: the drive lives in the flash (user decision)
#define BOARD_HAS_ETH         0
#define BOARD_HAS_WALLPAPER   0       // not with this much RAM
#define BOARD_HAS_CAMERA      0
#define BOARD_HAS_WIFI_REMOTE 0       // Wi-Fi is this chip's own radio
#define BOARD_CONSOLE_USB_JTAG 0      // console, logs and the command channel on UART0 (the COM port)

// Loadable apps may ask for at most this much PSRAM (the OS and its own buffers need the rest).
#define BOARD_APP_MEM_MAX     (6 * 1024 * 1024)

// VGA out through a resistor DAC: RGB222, two resistors per colour, on eight data lines, with
// separate sync on GPIO17/18.
#define BOARD_PIN_VGA_R_LO    5
#define BOARD_PIN_VGA_R_HI    4
#define BOARD_PIN_VGA_G_LO    7
#define BOARD_PIN_VGA_G_HI    6
#define BOARD_PIN_VGA_B_LO    16
#define BOARD_PIN_VGA_B_HI    15
#define BOARD_PIN_VGA_HSYNC   17
#define BOARD_PIN_VGA_VSYNC   18

// USB host (keyboard, mouse) on the native USB pins, wired to a USB-C breakout; the board's own
// USB connector must stay unplugged, and the devices need their own 5 V.
#define BOARD_PIN_USB_DM      19
#define BOARD_PIN_USB_DP      20
