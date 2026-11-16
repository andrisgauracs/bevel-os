// What the app loader needs from the platform: load a program image, run its entry point in the
// calling task, and turn a fault inside it into app_fault() + app_crash_landing().
//   app_elf.c   ESP32-S3: Espressif's elf_loader, Xtensa exception table
//   host/       desktop simulator: dlopen, POSIX signals
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../apps/bevel.h"

void app_plat_init(void);
void *app_plat_load(const char *path, const uint8_t *img, int len);   // NULL if it cannot be loaded
void app_plat_unload(void *image);
void app_plat_run(void *image, const bevel_api_t *api, const char *arg);

// Provided by app_loader.c.
uintptr_t app_libc_symbol(const char *name);   // C library functions an app may import (0 = none)
bool app_fault(uint32_t cause, uint32_t pc, uint32_t addr); // true: the current task is an app, redirect it
void app_crash_landing(void) __attribute__((noreturn));
