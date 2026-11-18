// Filesystem helpers shared by Files, the editor, the terminal and the app loader.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "wm.h"

typedef struct { char name[68]; uint32_t size; bool is_dir; } fs_entry_t;

void fs_join(char *out, int n, const char *dir, const char *name);
const char *fs_ext(const char *name);
const char *fs_basename(const char *path);
bool fs_exists(const char *path);
bool fs_is_dir(const char *path);
// Every part of the path below the drive exists spelt exactly so. FAT itself ignores case, so
// fs_exists("/disk/sd") is true while the card is mounted at "/disk/SD" only.
bool fs_exists_exact(const char *path);
int  fs_list(const char *dir, fs_entry_t **out);      // sorted, folders first; caller frees; -1 on error
bool fs_delete(const char *path);                     // recursive
bool fs_copy(const char *src, const char *dst);       // recursive
bool fs_valid_name(const char *name);
char *fs_read_all(const char *path, int *len);        // PSRAM, NUL-terminated; caller frees
const char *fs_type_name(const fs_entry_t *e);
const bitmap_t *fs_type_icon(const fs_entry_t *e);
void os_open_file(window_t *owner, const char *path); // open with the right app
void os_run_elf(const char *path);
