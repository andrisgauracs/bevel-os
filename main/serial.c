// Serial command channel on UART0 (the COM port): framebuffer dumps, input injection, stats and
// self-tests, so the OS can be driven and checked without anyone at the keyboard.
// Commands are text lines; every command ends its output with a line "@@OK" (or "@@ERR ...").
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "vga.h"
#include "comp.h"
#include "input.h"
#include "os.h"
#include "mbedtls/base64.h"

#if CONFIG_BEVEL_NET_OPENETH
// QEMU hands over input as fast as the host sends it, not at the line rate: room for a whole
// command line (putdata is 4.1 KB) so none of it is dropped while the command task is busy.
#define SERIAL_RX_BYTES 16384
#else
#define SERIAL_RX_BYTES 4096
#endif

static FILE *s_put;   // file being uploaded (putopen / putdata / putclose)

static void out(const char *s) { uart_write_bytes(UART_NUM_0, s, strlen(s)); }

void serial_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    uart_write_bytes(UART_NUM_0, buf, n);
}

// Framebuffer as RLE: header "@@FB w h bytes", then (count, value) pairs. Includes the pointer.
static void dump_fb(void *arg)
{
    const int len = VGA_W * VGA_H;
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) { out("@@ERR no memory\n"); return; }
    memcpy(copy, vga_fb(), len);
    if (!arg) vga_cursor_overlay(copy);
    size_t total = 0;
    for (int i = 0; i < len;) {
        int run = 1;
        while (i + run < len && run < 255 && copy[i + run] == copy[i]) run++;
        total += 2;
        i += run;
    }
    serial_printf("\n@@FB %d %d %u\n", VGA_W, VGA_H, (unsigned)total);
    static uint8_t buf[1024];
    int n = 0;
    for (int i = 0; i < len;) {
        int run = 1;
        while (i + run < len && run < 255 && copy[i + run] == copy[i]) run++;
        buf[n++] = (uint8_t)run;
        buf[n++] = copy[i];
        i += run;
        if (n == sizeof(buf)) { uart_write_bytes(UART_NUM_0, buf, n); n = 0; }
    }
    if (n) uart_write_bytes(UART_NUM_0, buf, n);
    out("\n@@END\n");
    free(copy);
}

static const struct { const char *name; uint8_t usage; } s_keynames[] = {
    {"enter", K_ENTER}, {"esc", K_ESC}, {"bksp", K_BKSP}, {"backspace", K_BKSP}, {"tab", K_TAB},
    {"space", K_SPACE}, {"ins", K_INS}, {"home", K_HOME}, {"pgup", K_PGUP}, {"del", K_DEL},
    {"delete", K_DEL}, {"end", K_END}, {"pgdn", K_PGDN}, {"right", K_RIGHT}, {"left", K_LEFT},
    {"down", K_DOWN}, {"up", K_UP}, {"menu", K_MENU}, {"gui", K_LGUI}, {"win", K_LGUI},
    {"ctrl", K_LCTRL}, {"shift", K_LSHIFT}, {"alt", K_LALT},
    {"f1", K_F1}, {"f2", K_F2}, {"f3", K_F3}, {"f4", K_F4}, {"f5", K_F5}, {"f6", K_F6},
    {"f7", K_F7}, {"f8", K_F8}, {"f9", K_F9}, {"f10", K_F10}, {"f11", K_F11}, {"f12", K_F12},
};

static uint8_t key_by_name(const char *s)
{
    for (int i = 0; i < (int)(sizeof(s_keynames) / sizeof(s_keynames[0])); i++) {
        if (!strcasecmp(s, s_keynames[i].name)) return s_keynames[i].usage;
    }
    if (strlen(s) == 1) {
        uint8_t u; bool sh;
        if (input_char_to_usage(tolower((unsigned char)s[0]), &u, &sh)) return u;
    }
    if (s[0] == '0' && s[1] == 'x') return (uint8_t)strtol(s, NULL, 16);
    return 0;
}

// "ctrl+alt+del", "alt+f4", "a", "shift+tab". mode: 0 = press and release, 1 = down, 2 = up
static bool do_key(char *spec, int mode)
{
    uint8_t seq[6];
    int n = 0;
    for (char *tok = strtok(spec, "+ "); tok && n < 6; tok = strtok(NULL, "+ ")) {
        uint8_t u = key_by_name(tok);
        if (!u) return false;
        seq[n++] = u;
    }
    if (!n) return false;
    if (mode != 2) for (int i = 0; i < n; i++) input_key(seq[i], true);
    if (mode != 1) for (int i = n - 1; i >= 0; i--) input_key(seq[i], false);
    return true;
}

static void do_type(const char *s)
{
    for (; *s; s++) {
        uint8_t u; bool sh;
        char c = *s;
        if (c == '\\' && s[1] == 'n') { c = '\n'; s++; }
        else if (c == '\\' && s[1] == 't') { c = '\t'; s++; }
        if (!input_char_to_usage(c, &u, &sh)) continue;
        if (sh) input_key(K_LSHIFT, true);
        input_key(u, true);
        input_key(u, false);
        if (sh) input_key(K_LSHIFT, false);
        // do not outrun the 64-entry event queue
        ev_call_sync(NULL, NULL);
    }
}

static void settle(void)
{
    // two round trips through the GUI task: events handled, then repainted
    ev_call_sync(NULL, NULL);
    ev_call_sync(NULL, NULL);
}

static void cmd_stat(void)
{
    vga_stats_t v;
    comp_stats_t c;
    vga_get_stats(&v, true);
    comp_get_stats(&c, true);
    serial_printf("vga: frames %u fills %u isr_avg %u.%u us isr_max %u us slack_min %d us late %u core %u copy %s\n",
                  (unsigned)v.frames, (unsigned)v.fills, (unsigned)(v.isr_avg_us10 / 10), (unsigned)(v.isr_avg_us10 % 10),
                  (unsigned)v.isr_max_us, (int)v.slack_min_us, (unsigned)v.late, (unsigned)v.isr_core,
                  vga_get_copy_mode() ? "pie" : "memcpy");
    serial_printf("comp: flushes %u pixels %u render %u us copy %u us wait %u us max_flush %u us\n",
                  (unsigned)c.flushes, (unsigned)c.pixels, (unsigned)c.render_us, (unsigned)c.copy_us,
                  (unsigned)c.wait_us, (unsigned)c.max_flush_us);
    serial_printf("mem: internal free %u largest %u min %u | psram free %u | uptime %u ms\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)(esp_timer_get_time() / 1000));
}

static void handle_line(char *line)
{
    while (*line == ' ') line++;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = line + strlen(line);
    bool ok = true;

    if (!strcmp(line, "D") || !strcmp(line, "dump")) {
        settle();
        ev_call_sync(dump_fb, (void *)(intptr_t)(!strcmp(arg, "nocursor")));
        return; // dump ends with @@END
    } else if (!strcmp(line, "stat")) {
        cmd_stat();
    } else if (!strcmp(line, "key")) {
        ok = do_key(arg, 0); settle();
    } else if (!strcmp(line, "keydown")) {
        ok = do_key(arg, 1); settle();
    } else if (!strcmp(line, "keyup")) {
        ok = do_key(arg, 2); settle();
    } else if (!strcmp(line, "type")) {
        do_type(arg); settle();
    } else if (!strcmp(line, "mouse")) {
        int x, y;
        if (sscanf(arg, "%d %d", &x, &y) == 2) { input_mouse_abs(x, y); settle(); } else ok = false;
    } else if (!strcmp(line, "mrel")) {
        int x, y;
        if (sscanf(arg, "%d %d", &x, &y) == 2) { input_mouse_rel(x, y); settle(); } else ok = false;
    } else if (!strcmp(line, "mdown") || !strcmp(line, "mup")) {
        int b = atoi(arg); if (!b) b = BTN_LEFT;
        uint8_t cur = input_buttons();
        input_mouse_buttons(line[1] == 'd' ? (cur | b) : (cur & ~b));
        settle();
    } else if (!strcmp(line, "click") || !strcmp(line, "dclick") || !strcmp(line, "rclick")) {
        int x, y;
        if (sscanf(arg, "%d %d", &x, &y) == 2) { input_mouse_abs(x, y); settle(); }
        int b = line[0] == 'r' ? BTN_RIGHT : BTN_LEFT;
        for (int i = 0; i < (line[0] == 'd' ? 2 : 1); i++) {
            input_mouse_buttons(b); settle();
            input_mouse_buttons(0); settle();
        }
    } else if (!strcmp(line, "wheel")) {
        input_wheel(atoi(arg)); settle();
    } else if (!strcmp(line, "copy")) {
        int m = atoi(arg); vga_set_copy_mode(m & 1); comp_copy_mode = m & 1;
    } else if (!strcmp(line, "race")) {
        comp_beam_race = atoi(arg);
    } else if (!strcmp(line, "redraw")) {
        ev_call_sync((void (*)(void *))comp_invalidate_all, NULL); settle();
    } else if (!strcmp(line, "reboot")) {
        out("@@OK\n"); uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100)); esp_restart();
    } else if (!strcmp(line, "putopen")) {
        if (s_put) fclose(s_put);
        s_put = fopen(arg, "wb");
        ok = s_put != NULL;
    } else if (!strcmp(line, "putdata")) {
        static uint8_t bin[3200];
        size_t n = 0;
        ok = s_put && mbedtls_base64_decode(bin, sizeof(bin), &n, (const unsigned char *)arg, strlen(arg)) == 0 &&
             fwrite(bin, 1, n, s_put) == n;
    } else if (!strcmp(line, "putat")) {
        // putat <offset> <base64>: idempotent (a repeated line writes the same bytes again), and
        // acknowledged with its offset and checksum, so a sender can retry over an unreliable
        // link (QEMU's emulated UART drops or repeats input under load)
        static uint8_t bin[3200];
        size_t n = 0;
        char *sp = strchr(arg, ' ');
        long off = atol(arg);
        ok = s_put && sp && mbedtls_base64_decode(bin, sizeof(bin), &n, (const unsigned char *)sp + 1, strlen(sp + 1)) == 0 &&
             fseek(s_put, off, SEEK_SET) == 0 && fwrite(bin, 1, n, s_put) == n;
        if (ok) {
            uint32_t s1 = 1, s2 = 0;   // Adler-32 of the bytes written, checked by the sender
            for (size_t i = 0; i < n; i++) { s1 = (s1 + bin[i]) % 65521; s2 = (s2 + s1) % 65521; }
            serial_printf("@@AT %ld %u\n", off, (unsigned)((s2 << 16) | s1));
        }
    } else if (!strcmp(line, "putclose")) {
        ok = s_put && fclose(s_put) == 0;
        s_put = NULL;
    } else if (!strcmp(line, "ping")) {
        out("pong\n");
    } else if (!os_serial_command(line, arg)) {
        ok = false;
    }
    out(ok ? "@@OK\n" : "@@ERR unknown or bad command\n");
}

static void serial_task(void *unused)
{
    static char line[4600];
    int n = 0;
    uint8_t c;
    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            line[n] = 0;
            if (n) handle_line(line);
            n = 0;
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = c;
        }
    }
}

void serial_start(void)
{
    uart_driver_install(UART_NUM_0, SERIAL_RX_BYTES, 0, 0, NULL, 0);
    xTaskCreatePinnedToCore(serial_task, "serial", 6144, NULL, 6, NULL, 0);
}
