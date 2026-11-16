// ESP-IDF system services for the desktop simulator: memory, time, log, restart, the serial
// command channel (on a TCP port) and the drive (a folder that appears as /disk).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include "host_port.h"
#include "host_sim.h"

// ---------------------------------------------------------------- errors, log

const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    }
    return "ESP_ERR";
}

void host_log(char level, const char *tag, const char *fmt, ...)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "%c (%u) %s: ", level, (unsigned)(esp_timer_get_time() / 1000), tag);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    uart_write_bytes(0, line, strlen(line));
}

// ---------------------------------------------------------------- memory

#define INTERNAL_TOTAL (320 * 1024)
#define PSRAM_TOTAL (8 * 1024 * 1024)

void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
void *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps)
{
    (void)caps;
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, n ? n : 1) == 0 ? p : NULL;
}
void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps)
{
    void *p = heap_caps_aligned_alloc(align, n * size, caps);
    if (p) memset(p, 0, n * size);
    return p;
}

// Plausible numbers for System Information: the board's sizes, minus what the simulator uses.
static size_t used_bytes(void) { struct mallinfo2 mi = mallinfo2(); return mi.uordblks; }
size_t heap_caps_get_total_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? PSRAM_TOTAL : INTERNAL_TOTAL; }
size_t heap_caps_get_free_size(uint32_t caps)
{
    if (!(caps & MALLOC_CAP_SPIRAM)) return INTERNAL_TOTAL / 2;
    size_t u = used_bytes();
    return u < PSRAM_TOTAL ? PSRAM_TOTAL - u : 0;
}
size_t heap_caps_get_largest_free_block(uint32_t caps) { return heap_caps_get_free_size(caps); }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps); }
size_t esp_psram_get_size(void) { return PSRAM_TOTAL; }

// ---------------------------------------------------------------- time, chip, misc

static int64_t s_t0;
__attribute__((constructor)) static void time_init(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_t0 = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - s_t0;
}

uint32_t esp_cpu_get_cycle_count(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(((uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec) * 24 / 100);
}

uint32_t esp_random(void)
{
    uint32_t v;
    if (getrandom(&v, sizeof(v), 0) != sizeof(v)) v = (uint32_t)rand();
    return v;
}

void esp_rom_delay_us(uint32_t us) { usleep(us); }

void esp_chip_info(esp_chip_info_t *out) { memset(out, 0, sizeof(*out)); out->revision = 2; out->cores = 2; }
esp_err_t esp_flash_get_size(void *chip, uint32_t *size) { (void)chip; *size = 16 * 1024 * 1024; return ESP_OK; }

// Changes with every build of the simulator, so the bundled programs are reinstalled on the drive.
int esp_app_get_elf_sha256(char *dst, size_t size)
{
    struct stat st;
    long long stamp = stat("/proc/self/exe", &st) == 0 ? (long long)st.st_mtime : 0;
    snprintf(dst, size, "sim-%lld", stamp);
    return (int)strlen(dst);
}

esp_err_t esp_ipc_call_blocking(uint32_t cpu, esp_ipc_func_t fn, void *arg) { (void)cpu; fn(arg); return ESP_OK; }

void esp_restart(void)
{
    host_log('I', "sim", "restarting");
    // descriptors would survive exec and keep the ports busy
    for (int fd = 3; fd < 1024; fd++) close(fd);
    execv("/proc/self/exe", host_argv);
    perror("execv");
    _exit(1);
}

size_t strlcpy(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(dst, src, k); dst[k] = 0; }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t n)
{
    size_t d = strnlen(dst, n);
    if (d == n) return n + strlen(src);
    return d + strlcpy(dst + d, src, n - d);
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src, size_t slen)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        const char *p = memchr(tbl, c, 64);
        if (!p) return -0x2C;   // MBEDTLS_ERR_BASE64_INVALID_CHARACTER
        acc = (acc << 6) | (uint32_t)(p - tbl);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= dlen) return -0x2A;   // MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL
            dst[n++] = (unsigned char)(acc >> bits);
        }
    }
    *olen = n;
    return 0;
}

// ---------------------------------------------------------------- serial channel over TCP
// One client at a time (tools/bv.py daemon). Everything written to "UART0" also goes to stdout.

static pthread_mutex_t s_ser_m = PTHREAD_MUTEX_INITIALIZER;
static int s_ser_fd = -1;
static uint8_t s_rx[8192];
static int s_rx_head, s_rx_n;
static sem_t s_rx_sem;

static void *serial_accept(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_mutex_lock(&s_ser_m);
        if (s_ser_fd >= 0) shutdown(s_ser_fd, SHUT_RDWR);
        s_ser_fd = fd;
        pthread_mutex_unlock(&s_ser_m);
        uint8_t buf[1024];
        for (;;) {
            int r = recv(fd, buf, sizeof(buf), 0);
            if (r <= 0) break;
            pthread_mutex_lock(&s_ser_m);
            for (int i = 0; i < r && s_rx_n < (int)sizeof(s_rx); i++) {
                s_rx[(s_rx_head + s_rx_n++) % sizeof(s_rx)] = buf[i];
                sem_post(&s_rx_sem);
            }
            pthread_mutex_unlock(&s_ser_m);
        }
        pthread_mutex_lock(&s_ser_m);
        if (s_ser_fd == fd) s_ser_fd = -1;
        pthread_mutex_unlock(&s_ser_m);
        close(fd);
    }
    return NULL;
}

int uart_driver_install(int port, int rx, int tx, int qsize, void *queue, int flags)
{
    (void)port; (void)rx; (void)tx; (void)qsize; (void)queue; (void)flags;
    sem_init(&s_rx_sem, 0, 0);
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(host_serial_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 1) != 0) {
        fprintf(stderr, "sim: serial channel: cannot listen on 127.0.0.1:%d (%s)\n", host_serial_port, strerror(errno));
        close(lfd);
        return ESP_FAIL;
    }
    fprintf(stderr, "sim: serial channel on 127.0.0.1:%d (BV_PORT=tcp:127.0.0.1:%d tools/bv.py ...)\n", host_serial_port, host_serial_port);
    pthread_t th;
    pthread_create(&th, NULL, serial_accept, (void *)(intptr_t)lfd);
    pthread_detach(th);
    return ESP_OK;
}

int uart_read_bytes(int port, void *buf, uint32_t len, TickType_t wait)
{
    (void)port;
    uint8_t *p = buf;
    uint32_t n = 0;
    while (n < len) {
        struct timespec ts;
        bool got;
        if (wait == portMAX_DELAY) got = sem_wait(&s_rx_sem) == 0;
        else {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)(wait % 1000) * 1000000; ts.tv_sec += wait / 1000 + ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000;
            got = sem_timedwait(&s_rx_sem, &ts) == 0;
        }
        if (!got) { if (errno == EINTR) continue; break; }
        pthread_mutex_lock(&s_ser_m);
        p[n++] = s_rx[s_rx_head];
        s_rx_head = (s_rx_head + 1) % sizeof(s_rx);
        s_rx_n--;
        pthread_mutex_unlock(&s_ser_m);
    }
    return (int)n;
}

int uart_write_bytes(int port, const void *buf, size_t len)
{
    (void)port;
    pthread_mutex_lock(&s_ser_m);
    if (host_echo_serial) fwrite(buf, 1, len, stdout), fflush(stdout);
    if (s_ser_fd >= 0) {
        const char *p = buf;
        size_t off = 0;
        while (off < len) {
            ssize_t r = send(s_ser_fd, p + off, len - off, MSG_NOSIGNAL);
            if (r <= 0) break;
            off += r;
        }
    }
    pthread_mutex_unlock(&s_ser_m);
    return (int)len;
}

esp_err_t uart_wait_tx_done(int port, TickType_t wait) { (void)port; (void)wait; return ESP_OK; }

// ---------------------------------------------------------------- drive

const char *host_disk_dir(void) { return host_disk; }

static void mkdirs(const char *path)
{
    char tmp[1024];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    mkdir(tmp, 0777);
}

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base, const char *label, const esp_vfs_fat_mount_config_t *cfg, wl_handle_t *wl)
{
    (void)base; (void)label; (void)cfg;
    mkdirs(host_disk);
    *wl = 1;
    struct stat st;
    return stat(host_disk, &st) == 0 && S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_vfs_fat_spiflash_unmount_rw_wl(const char *base, wl_handle_t wl) { (void)base; (void)wl; return ESP_OK; }

esp_err_t esp_vfs_fat_info(const char *base, uint64_t *total, uint64_t *free_bytes)
{
    (void)base;
    // show the board's 12 MB drive, with the space the files in the folder take
    struct statvfs v;
    if (statvfs(host_disk, &v) != 0) return ESP_FAIL;
    *total = 12 * 1024 * 1024;
    uint64_t host_free = (uint64_t)v.f_bavail * v.f_frsize;
    *free_bytes = host_free < *total ? host_free : *total - host_disk_used();
    return ESP_OK;
}
