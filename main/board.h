// The board this firmware was built for, in one place: which chip it carries, what it has, and which
// pins everything hangs off. Two boards are supported:
//
//   board_p4.h - Guition JC1060P470-V2 (ESP32-P4 + ESP32-C6): 7" MIPI-DSI panel, touch, Ethernet,
//                microSD, RTC, audio through the JP1 header. Only the -V2 revision works: the other
//                JC1060P470 versions do not drive the same panel.
//   board_s3.h - ESP32-S3 devkit (QFN56 v0.2) with a resistor DAC into a VGA monitor, keyboard and
//                mouse on a USB-C breakout, no other peripherals wired.
//
// Code that only differs by what is wired up tests the BOARD_HAS_* flags below. Code that differs by
// CPU (exception frames, the USB controller's registers, PIE instructions) tests
// CONFIG_IDF_TARGET_ESP32S3 / CONFIG_IDF_TARGET_ESP32P4.
#pragma once
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "board_p4.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "board_s3.h"
#else
#error "Bevel OS builds for the ESP32-P4 (Guition JC1060P470-V2) and the ESP32-S3 VGA devkit: idf.py set-target esp32p4 | esp32s3"
#endif

#if BOARD_HAS_I2C
#include "driver/i2c_master.h"

// The one I2C bus, created on first use (board.c). The VGA devkit has nothing on I2C at all, so it
// has no bus: touch, the clock chip and the camera are all P4 wiring.
i2c_master_bus_handle_t board_i2c(void);
#endif
