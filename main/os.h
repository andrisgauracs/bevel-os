// Shared declarations for Bevel OS.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"
#include "theme.h"
#include "board.h"

#define OS_NAME "Bevel OS"
#define OS_VERSION "1.0"
#define OS_CHIP BOARD_CHIP

// Which core runs what. The GUI task (window manager, compositor, built-in apps) lives on
// GUI_CORE; USB, SD card, serial and loaded apps live on the other one.
#define GUI_CORE 1
#define SYS_CORE 0

// serial.c
void serial_start(void);
void serial_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool os_serial_command(const char *cmd, char *arg); // extra commands; false = unknown

// perf.c
typedef struct { char name[28]; uint32_t value; const char *unit; } perf_result_t;
#define PERF_MAX 48
extern perf_result_t perf_results[PERF_MAX];
extern int perf_count;
void perf_run(const char *which);   // call in the GUI task; "all" or a group name
uint32_t os_uptime_ms(void);

// Hooks filled in by later subsystems (storage, loaded apps)
void os_disk_summary(char *buf, int n);     // the internal drive: "9.1 MB free of 11.9 MB (flash)"
void os_sd_summary(char *buf, int n);       // "1.9 GB free of 3.7 GB" / "no card"
int  os_task_mem_kb(int task_id);           // -1 for system windows
bool os_task_kill(int task_id);             // false if task_id is not a loaded app

// storage.c
#define DISK_ROOT "/disk"
#if BOARD_HAS_SD
#define SD_ROOT DISK_ROOT "/SD"            // where a microSD card appears
#endif
#define USB_ROOT DISK_ROOT "/USB"          // where a USB stick appears
bool storage_sd_ok(void);                  // false on boards without a card reader
bool storage_is_mount_point(const char *path);  // the drive, or the folder a card or stick is mounted on
const char *storage_volume_of(const char *path); // SD_ROOT or USB_ROOT if path is on that mounted volume, else NULL
bool storage_path_gone(const char *path);       // path is where a card or stick would be, and none is mounted
bool storage_eject(const char *volume);         // volume as returned by storage_volume_of
bool storage_init(void);
bool storage_ok(void);
bool storage_df(uint64_t *total, uint64_t *free_bytes);
bool storage_df_path(const char *path, uint64_t *total, uint64_t *free_bytes);  // of the volume holding path
void storage_unmount(void);
void os_fmt_size(char *buf, int n, uint64_t bytes);

// usb_storage.c
bool storage_usb_ok(void);                 // a stick is mounted at USB_ROOT
bool usb_storage_eject(void);              // unmount it; false if there was none
void os_usb_summary(char *buf, int n);     // "812.0 MB free of 960.0 MB" / "no stick"

#if BOARD_HAS_WIFI_REMOTE
// main.c: ESP-Hosted has its SDIO link to the ESP32-C6 running. The card slot and the C6 share one
// SDMMC controller, so the card task waits for this before touching the slot.
extern volatile bool os_c6_link_up;
#endif

// clock.c
void clock_init(void);                      // reads the clock chip, if there is one
bool clock_valid(void);                     // the time of day is known
const char *clock_source(void);
int  clock_zone_count(void);
const char *clock_zone_name(int i);
int  clock_zone(void);
void clock_set_zone(int i);

// Grow a PSRAM block. Unlike realloc, the copy happens outside the allocator's lock, so a large
// block never stalls interrupts (the display's scan-out) while it is moved.
void *os_grow(void *old, size_t old_bytes, size_t new_bytes);
