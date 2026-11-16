// The drive: the OS uses paths under /disk, which live in a folder of the host (--disk). The
// file functions the OS calls are wrapped at link time (-Wl,--wrap=...) to rewrite those paths.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include "host_sim.h"

const char *host_path(const char *path, char *buf, int n)
{
    if (!path || strncmp(path, "/disk", 5) || (path[5] && path[5] != '/')) return path;
    snprintf(buf, n, "%s%s", host_disk, path + 5);
    return buf;
}

#define MAP(p) char _b_##p[1200]; p = host_path(p, _b_##p, sizeof(_b_##p))

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) { MAP(path); return __real_fopen(path, mode); }

DIR *__real_opendir(const char *path);
DIR *__wrap_opendir(const char *path) { MAP(path); return __real_opendir(path); }

int __real_stat(const char *path, struct stat *st);
int __wrap_stat(const char *path, struct stat *st) { MAP(path); return __real_stat(path, st); }

int __real_mkdir(const char *path, mode_t mode);
int __wrap_mkdir(const char *path, mode_t mode) { MAP(path); return __real_mkdir(path, mode); }

int __real_rmdir(const char *path);
int __wrap_rmdir(const char *path) { MAP(path); return __real_rmdir(path); }

int __real_unlink(const char *path);
int __wrap_unlink(const char *path) { MAP(path); return __real_unlink(path); }

int __real_remove(const char *path);
int __wrap_remove(const char *path) { MAP(path); return __real_remove(path); }

int __real_rename(const char *a, const char *b);
int __wrap_rename(const char *a, const char *b) { MAP(a); MAP(b); return __real_rename(a, b); }

static uint64_t s_used;
static int add_size(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)p; (void)f;
    if (flag == FTW_F) s_used += ((uint64_t)st->st_size + 4095) & ~4095ull;
    return 0;
}

uint64_t host_disk_used(void)
{
    s_used = 0;
    nftw(host_disk, add_size, 16, FTW_PHYS);
    return s_used;
}
