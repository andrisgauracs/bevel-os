// Desktop simulator: settings from the command line, shared by the host/ files.
#pragma once
#include <stdint.h>
#include <stdbool.h>

extern char **host_argv;          // for esp_restart (re-exec)
extern char host_disk[1024];      // the folder that appears as /disk
extern int host_serial_port;      // TCP port of the serial command channel
extern bool host_echo_serial;     // copy the serial output (log) to stdout
uint64_t host_disk_used(void);    // bytes used by the files on the drive

// Maps an OS path ("/disk/...") to the host folder. Returns path itself when it is not on /disk.
const char *host_path(const char *path, char *buf, int n);
