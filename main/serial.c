// Serial command channel on whichever port the board is wired to: the P4 board's USB-Serial-JTAG
// port ("USB1", also the log console and the flashing port), the S3 board's UART0 COM port at
// 2 Mbaud. Screen dumps, input injection, stats and self-tests, so the OS can be driven and checked
// without anyone at the keyboard.
// Commands are text lines; every command ends its output with a line "@@OK" (or "@@ERR ...").
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"      // BOARD_CONSOLE_USB_JTAG: which port this board is wired to
#if BOARD_CONSOLE_USB_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#endif
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "display.h"
#include "comp.h"
#include "input.h"
#include "layout.h"
#include "apps.h"
#include "os.h"
#include "mbedtls/base64.h"

static FILE *s_put;   // file being uploaded (putopen / putdata / putclose)

#if BOARD_CONSOLE_USB_JTAG
// Nothing is sent while no computer has the port open, so a missing listener never stalls the OS.
static void out_bytes(const void *data, int n)
{
    const uint8_t *p = data;
    while (n > 0 && usb_serial_jtag_is_connected()) {
        int k = usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(200));
        if (k <= 0) break;
        p += k; n -= k;
    }
}

// Before a reboot: the port needs a moment to flush what is queued.
static void out_drain(void) { vTaskDelay(pdMS_TO_TICKS(100)); }

static bool in_byte(uint8_t *c) { return usb_serial_jtag_read_bytes(c, 1, portMAX_DELAY) == 1; }
#else
static void out_bytes(const void *data, int n) { uart_write_bytes(UART_NUM_0, (const char *)data, n); }
static void out_drain(void) { uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100)); }
static bool in_byte(uint8_t *c) { return uart_read_bytes(UART_NUM_0, c, 1, portMAX_DELAY) == 1; }
#endif

static void out(const char *s) { out_bytes(s, strlen(s)); }

void serial_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    out_bytes(buf, n);
}

// The screen as it looks (RGB565, pointer and overlays included), run-length encoded:
// header "@@FB16 w h bytes", then (count, low byte, high byte) triplets.
static void dump_fb(void *arg)
{
    const int len = SCREEN_W * SCREEN_H;
    uint16_t *copy = heap_caps_malloc(len * 2, MALLOC_CAP_SPIRAM);
    if (!copy) { out("@@ERR no memory\n"); return; }
    disp_snapshot(copy, arg != NULL);
    size_t total = 0;
    for (int i = 0; i < len;) {
        int run = 1;
        while (i + run < len && run < 255 && copy[i + run] == copy[i]) run++;
        total += 3;
        i += run;
    }
    serial_printf("\n@@FB16 %d %d %u\n", SCREEN_W, SCREEN_H, (unsigned)total);
    static uint8_t buf[1020];
    int n = 0;
    for (int i = 0; i < len;) {
        int run = 1;
        while (i + run < len && run < 255 && copy[i + run] == copy[i]) run++;
        buf[n++] = (uint8_t)run;
        buf[n++] = (uint8_t)copy[i];
        buf[n++] = (uint8_t)(copy[i] >> 8);
        i += run;
        if (n == sizeof(buf)) { out_bytes(buf, n); n = 0; }
    }
    if (n) out_bytes(buf, n);
    out("\n@@END\n");
    free(copy);
}

static const struct { const char *name; uint8_t usage; } s_keynames[] = {
    {"enter", K_ENTER}, {"esc", K_ESC}, {"bksp", K_BKSP}, {"backspace", K_BKSP}, {"tab", K_TAB},
    {"space", K_SPACE}, {"ins", K_INS}, {"home", K_HOME}, {"pgup", K_PGUP}, {"del", K_DEL},
    {"delete", K_DEL}, {"end", K_END}, {"pgdn", K_PGDN}, {"right", K_RIGHT}, {"left", K_LEFT},
    {"down", K_DOWN}, {"up", K_UP}, {"menu", K_MENU}, {"gui", K_LGUI}, {"win", K_LGUI},
    {"ctrl", K_LCTRL}, {"shift", K_LSHIFT}, {"alt", K_LALT}, {"lalt", K_LALT}, {"ralt", K_RALT},
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
    for (int i = 0; s[i]; ) {
        unsigned char c = (unsigned char)s[i];
        uint16_t code;
        int n = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) != 0xC0 || !s[i + 1]) { i++; continue; }  // text this board cannot type
            code = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            n = 2;
        } else {
            code = c;
            if (c == '\\' && (s[i + 1] == 'n' || s[i + 1] == 't')) { code = s[i + 1] == 'n' ? '\n' : '\t'; n = 2; }
        }
        uint8_t u = 0;
        int st = 0;
        if (code == '\n') u = K_ENTER;
        else if (code == '\t') u = K_TAB;
        else if (!input_key_find(code, &u, &st)) { i += n; continue; }  // not on this keyboard
        if (st & 2) input_key(K_RALT, true);
        if (st & 1) input_key(K_LSHIFT, true);
        input_key(u, true);
        input_key(u, false);
        if (st & 1) input_key(K_LSHIFT, false);
        if (st & 2) input_key(K_RALT, false);
        i += n;
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
    comp_stats_t c;
    comp_get_stats(&c, true);
#if DISP_HAS_SCANOUT
    disp_scanout_t v;
    disp_get_scanout(&v, true);
    serial_printf("vga: frames %u fills %u isr_avg %u.%u us isr_max %u us slack_min %d us late %u core %u copy %s\n",
                  (unsigned)v.frames, (unsigned)v.fills, (unsigned)(v.isr_avg_us10 / 10), (unsigned)(v.isr_avg_us10 % 10),
                  (unsigned)v.isr_max_us, (int)v.slack_min_us, (unsigned)v.late, (unsigned)v.isr_core,
                  disp_get_copy_mode() ? "pie" : "memcpy");
    serial_printf("comp: flushes %u pixels %u render %u us copy %u us wait %u us max_flush %u us\n",
                  (unsigned)c.flushes, (unsigned)c.pixels, (unsigned)c.render_us, (unsigned)c.copy_us,
                  (unsigned)c.wait_us, (unsigned)c.max_flush_us);
#else
    serial_printf("comp: flushes %u pixels %u render %u us copy %u us max_flush %u us\n",
                  (unsigned)c.flushes, (unsigned)c.pixels, (unsigned)c.render_us, (unsigned)c.copy_us,
                  (unsigned)c.max_flush_us);
#endif
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
#if DISP_HAS_SCANOUT
    } else if (!strcmp(line, "copy")) {
        disp_set_copy_mode(atoi(arg) & 1);
    } else if (!strcmp(line, "race")) {
        comp_beam_race = atoi(arg);
#endif
    } else if (!strcmp(line, "redraw")) {
        ev_call_sync((void (*)(void *))comp_invalidate_all, NULL); settle();
    } else if (!strcmp(line, "reboot")) {
        out("@@OK\n"); out_drain(); esp_restart();
    } else if (!strcmp(line, "putopen")) {
        if (s_put) fclose(s_put);
        s_put = fopen(arg, "wb");
        ok = s_put != NULL;
    } else if (!strcmp(line, "putdata")) {
        static uint8_t bin[3200];
        size_t n = 0;
        ok = s_put && mbedtls_base64_decode(bin, sizeof(bin), &n, (const unsigned char *)arg, strlen(arg)) == 0 &&
             fwrite(bin, 1, n, s_put) == n;
    } else if (!strcmp(line, "putclose")) {
        ok = s_put && fclose(s_put) == 0;
        s_put = NULL;
    } else if (!strcmp(line, "layout")) {
        serial_printf("layout: %d %s\n", input_layout_get(), layout_name[input_layout_get()]);
    } else if (!strcmp(line, "keytext")) {
        char out[8] = { 0 };
        char *tok = strtok(arg, "+ ");
        uint8_t u = key_by_name(tok);
        if (!u) { ok = false; }
        else {
            uint8_t mods = 0;
            for (tok = strtok(NULL, "+ "); tok; tok = strtok(NULL, "+ ")) {
                uint8_t m = key_by_name(tok);
                if (m == K_RALT) mods |= MOD_RALT;  // the state bits key_text reads
                if (m == K_LSHIFT || m == K_RSHIFT) mods |= MOD_SHIFT;
            }
            input_key_text_test(u, mods, out, sizeof out);
            serial_printf("key %u mod %u: %d bytes", u, mods, (int)strlen(out));
            for (int i = 0; out[i]; i++) serial_printf(" %02X", (unsigned char)out[i]);
            serial_printf("\n");
        }
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
        if (!in_byte(&c)) continue;
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
#if BOARD_CONSOLE_USB_JTAG
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 4096, .rx_buffer_size = 8192};
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();   // the log console shares the port through the same driver
#else
    uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);
#endif
    xTaskCreatePinnedToCore(serial_task, "serial", 6144, NULL, 6, NULL, 0);
}
