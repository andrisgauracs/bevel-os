// Built-in application registry and launching.
#pragma once
#include "wm.h"

#define APP_ON_DESKTOP  1
#define APP_IN_PROGRAMS 2   // listed under Programs instead of the top of the main menu
#define APP_HIDDEN      4   // not in menus (opened by file type or shortcut)

typedef struct {
    const char *name;           // unique, also the desktop label
    const char *menu_label;     // with mnemonic
    const bitmap_t *icon16, *icon32;
    void (*launch)(const char *arg);
    int flags;
} app_def_t;

extern const app_def_t os_apps[];
extern const int os_app_count;
void app_launch(int idx, const char *arg);
void app_launch_name(const char *name, const char *arg);

// Apps found on the SD card (/sd/apps/*.elf)
int app_sd_count(void);
const char *app_sd_name(int i);
bool app_sd_is_game(int i);
void app_launch_sd(int i);
void app_sd_rescan(void);

void os_shutdown_prepare(void);     // save settings, unmount the card

// launch functions
void app_system(const char *arg);
void app_tasks(const char *arg);
void app_help(const char *arg);
void app_bench(const char *arg);
void app_gallery(const char *arg);
void app_files(const char *arg);
void app_editor(const char *arg);
void app_terminal(const char *arg);
void app_settings(const char *arg);
void app_viewer(const char *arg);
void app_browser(const char *arg);
void app_network(const char *arg);
void app_player(const char *arg);
void app_camera(const char *arg);
