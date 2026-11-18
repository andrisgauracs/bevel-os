// Boot splash: a black screen with the logo, the name of the system and a blocky progress bar,
// drawn in the same 64-colour palette and with the same bevelled frames as the desktop. It owns the
// logical framebuffer from boot_init() until boot_done(); see boot.h for why nothing else may touch
// the display in that window.
#include <stdio.h>
#include <string.h>
#include "boot.h"
#include "display.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "input.h"
#include "os.h"
#include "assets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "boot";

#if BOARD_HAS_TOUCH
#define STAGE_TOUCH_NAME  "Touch panel"
#else
#define STAGE_TOUCH_NAME  "No touch panel"
#endif
#if BOARD_HAS_AUDIO
#define STAGE_SOUND_NAME  "Sound"
#else
#define STAGE_SOUND_NAME  "No sound hardware"
#endif

static const char *const s_stage_name[BOOT_STAGE_COUNT] = {
    "Drive", "Settings", "Clock", "Desktop", "Programs",
    STAGE_TOUCH_NAME, STAGE_SOUND_NAME, "Network", "Input devices",
};

// Panel geometry, kept away from the edges so the same drawing works on 640x480 and 1024x600.
#define PANEL_W   420
#define PANEL_H   280
#define PANEL_X   ((SCREEN_W - PANEL_W) / 2)
#define PANEL_Y   ((SCREEN_H - PANEL_H) / 2 - 10)
#define INNER_X   (PANEL_X + 16)
#define INNER_W   (PANEL_W - 32)
#define BAR_H     20
#define BLOCK_W   16            // one block of the bar, 1 px gap between blocks
#define MSG_W     40            // status line, in whole mono cells: the per-cent read-out sits
#define EXTRA_W   20            //   to its right, so both are clipped to fit

static const pixel_t s_dash_pat[4] = { C_GREY, C_GREY, C_BLACK, C_BLACK };

static int s_stage, s_pct, s_fill;
static uint32_t s_splash_ms, s_gone_ms = (uint32_t)-1, s_ready_ms;
static char s_msg[MSG_W + 1];
static char s_extra[EXTRA_W + 1];
static bool s_dirty;

// usb_input.c: what the USB host has enumerated so far, and how much of anything it has mounted
extern int usb_keyboards(void);
extern int usb_mice(void);
extern int usb_devices_mounted(void);
// usb_input.c: the port watchdog. usb_boot_wait tells it not to back off while the splash waits for
// a pointer, and usb_force_cycle asks for one fresh connection attempt on the root port.
extern volatile int usb_boot_wait, usb_force_cycle;

// ---------------------------------------------------------------- drawing

static void text2(gfx_t *g, int x, int y, const char *s, pixel_t c, int step)
{
    for (; *s; s++, x += 16 * step) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) continue;
        const uint8_t *gl = font_mono[(int)ch - 32];
        for (int j = 0; j < 16; j++) {
            uint8_t bits = gl[j];
            if (!bits) continue;
            for (int col = 0; col < 8; col++) {
                if (!(bits & (0x80 >> col))) continue;
                for (int dy = 0; dy < step; dy++)
                    gfx_fill(g, x + col * step, y + j * step, step, step, c);
            }
        }
    }
}

static void shadow_text2(gfx_t *g, int x, int y, const char *s)
{
    text2(g, x + 2, y + 2, s, C_DKGREY, 2);
    text2(g, x, y, s, C_WHITE, 2);
}

static void dashed_hline(gfx_t *g, int x, int y, int w, const pixel_t *pat)
{
    for (int i = 0; i < w; i++) gfx_hline(g, x + i, y, 1, pat[i & 3]);
}

static void put4(gfx_t *g, int x, int y, pixel_t c) { gfx_fill(g, x, y, 4, 4, c); }

static void boot_splash(void)
{
    gfx_t g;
    gfx_init(&g, disp_fb(), SCREEN_W, 0, 0, SCREEN_W, SCREEN_H);

    // the whole screen: black with a checker edge, the way a CRT blanking looks
    gfx_fill(&g, 0, 0, SCREEN_W, SCREEN_H, C_BLACK);
    gfx_checker(&g, 0, 0, SCREEN_W, 2, C_DKGREY, C_BLACK);
    gfx_checker(&g, 0, SCREEN_H - 2, SCREEN_W, 2, C_DKGREY, C_BLACK);
    gfx_checker(&g, 0, 0, 2, SCREEN_H, C_DKGREY, C_BLACK);
    gfx_checker(&g, SCREEN_W - 2, 0, 2, SCREEN_H, C_DKGREY, C_BLACK);

    gfx_fill(&g, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, C_BLACK);
    gfx_frame(&g, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, C_GREY);
    put4(&g, PANEL_X - 2, PANEL_Y - 2, C_GREY);
    put4(&g, PANEL_X + PANEL_W - 2, PANEL_Y - 2, C_GREY);
    put4(&g, PANEL_X - 2, PANEL_Y + PANEL_H - 2, C_GREY);
    put4(&g, PANEL_X + PANEL_W - 2, PANEL_Y + PANEL_H - 2, C_GREY);

    gfx_bitmap(&g, PANEL_X + (PANEL_W - 32) / 2, PANEL_Y + 18, &icon32_logo);
    // the name in 2x mono: a banner 32 px high, and the version and chip on the line below
    shadow_text2(&g, PANEL_X + (PANEL_W - 32 * (int)strlen(OS_NAME) - 2) / 2, PANEL_Y + 56, OS_NAME);
    dashed_hline(&g, INNER_X, PANEL_Y + 108, INNER_W, s_dash_pat);

    // the bar: a sunken field filled with blocks, one sixteenth of its width each
    int bx = PANEL_X + 16, by = PANEL_Y + 122;
    ui_draw_sunken(&g, bx, by, INNER_W, BAR_H);
    for (int i = 0; i < s_fill; i++) {
        int x0 = bx + 4 + i * (BLOCK_W + 1);
        gfx_fill(&g, x0, by + 4, BLOCK_W, BAR_H - 8, C_CYAN);
        gfx_hline(&g, x0, by + 4, BLOCK_W, C_WHITE);
        gfx_vline(&g, x0, by + 4, BAR_H - 8, C_WHITE);
        gfx_hline(&g, x0, by + BAR_H - 5, BLOCK_W, C_DKGREY);
        gfx_vline(&g, x0 + BLOCK_W - 1, by + 4, BAR_H - 8, C_DKGREY);
    }
    char line[96];
    snprintf(line, sizeof(line), "%d%%", s_pct);
    gfx_text(&g, PANEL_X + PANEL_W - 16 - gfx_text_w(line, -1, FONT_MONO), by + BAR_H + 4, line, -1, FONT_MONO, C_GREY);
    gfx_text(&g, PANEL_X + 16, by + BAR_H + 4, s_msg, -1, FONT_MONO, C_WHITE);

    snprintf(line, sizeof(line), "(%d/%d) %s", s_stage + 1, BOOT_STAGE_COUNT, s_stage_name[s_stage]);
    gfx_text(&g, PANEL_X + 16, by + BAR_H + 22, line, -1, FONT_MONO, C_GREY);

    dashed_hline(&g, INNER_X, PANEL_Y + 208, INNER_W, s_dash_pat);
    snprintf(line, sizeof(line), "%s %s  %s", OS_NAME, OS_VERSION, BOARD_CHIP);
    gfx_text(&g, PANEL_X + 16, PANEL_Y + 222, line, -1, FONT_MONO, C_DKGREY);
    snprintf(line, sizeof(line), "now %5u ms", (unsigned)os_uptime_ms());
    gfx_text(&g, PANEL_X + PANEL_W - 16 - gfx_text_w(line, -1, FONT_MONO), PANEL_Y + 222, line, -1, FONT_MONO, C_GREY);
    snprintf(line, sizeof(line), "no mouse? Tab / Enter / start menu");
    gfx_text(&g, PANEL_X + 16, PANEL_Y + 240, line, -1, FONT_MONO, C_DKGREY);
}

static void flush_splash(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    boot_splash();
    disp_present(0, 0, SCREEN_W, SCREEN_H);
}

// ---------------------------------------------------------------- progress

// The status line names the stage and, when there is one, what is happening inside it.
static void set_msg(int stage, const char *detail)
{
    const char *name = s_stage_name[stage];
    if (detail && detail[0]) {
        snprintf(s_extra, sizeof(s_extra), "%.*s", EXTRA_W, detail);
        snprintf(s_msg, sizeof(s_msg), "%s (%.*s)", name, EXTRA_W, s_extra);
    } else {
        snprintf(s_msg, sizeof(s_msg), "%.*s", MSG_W, name);
    }
    s_dirty = true;
}

// Each stage owns one ninth of the bar: index/total of that ninth, so the bar also says how far
// inside a slow stage (rewriting the drive, waiting for a mouse) we are.
static void set_pct(int stage, int index, int total)
{
    int pct = (stage * 100 + (total > 0 ? 100 * index / total : 0)) / BOOT_STAGE_COUNT;
    int fill = (INNER_W - 8) * pct / 100 / (BLOCK_W + 1);
    if (pct == s_pct && fill == s_fill) return;
    s_pct = pct;
    s_fill = fill;
    s_dirty = true;
}

void boot_init(void)
{
    s_splash_ms = os_uptime_ms();
    set_msg(BOOT_STAGE_DRIVE, "mounting");
    set_pct(BOOT_STAGE_DRIVE, 0, 1);
    flush_splash();
    ESP_LOGI(TAG, "splash up at %u ms", (unsigned)s_splash_ms);
}

void boot_stage(int stage)
{
    if (stage < 0 || stage >= BOOT_STAGE_COUNT) return;
    s_stage = stage;
    set_msg(stage, NULL);
    set_pct(stage, 0, 1);
    flush_splash();
}

void boot_sub(int stage, int index, int total, const char *detail)
{
    if (stage < 0 || stage >= BOOT_STAGE_COUNT || index < 0 || total <= 0) return;
    if (index > total) index = total;
    s_stage = stage;
    set_msg(stage, detail);
    set_pct(stage, index, total);
    flush_splash();
}

// Everything the desktop is made of is up, but nothing can be clicked until the USB host has
// enumerated a keyboard or a mouse (a touchscreen or a JetKVM's digitiser answer sooner, through
// input_mouse_seen). The bus says which of two cases this is, and they want different waits:
//
//   Nothing mounted. Give up after timeout_ms and show the desktop: it works from the keyboard with
//   Tab and Enter, and holding the bar up longer only makes the boot slower for nobody's benefit.
//
//   Something is mounted but it is not a keyboard or a mouse - a hub, say. Its children turn up
//   late (measured on the bench: the hub answers around 1.7 s after reset, the composite device
//   behind it finishes enumerating at 5.7 s), so keep waiting, up to HUB_EXTRA_MS longer. A hub
//   with nothing useful behind it pays that wait in full; the status line says so while it runs
//   ("bus: 2, no pointer"), which beats a cursor that moves two seconds after the desktop does.
//
// Whichever wait applies, the bar runs to its end, so it never sits at 100% while still waiting.
#define HUB_EXTRA_MS 4000    // give devices behind a hub this much more time
#define NUDGE_MS     1200    // nothing on the bus after this long: ask for a fresh connection attempt

void boot_wait_input(int timeout_ms)
{
    static const char spin[] = "|/-\\";
    uint32_t t0 = os_uptime_ms();
    int spin_i = 0, mounted = -1;
    bool nudged = false;
    bool have_mouse = input_mouse_seen();
    usb_boot_wait = 1;   // the port watchdog keeps its short cycle interval while this is set
    for (;;) {
        int kb = usb_keyboards(), ms = usb_mice();
        int dev = usb_devices_mounted();
        if (dev != mounted) mounted = dev;
        int waited = (int)(os_uptime_ms() - t0);
        bool ready = have_mouse || kb || ms;
        int cap = timeout_ms > 0 ? timeout_ms : 1;
        if (mounted > 0) cap += HUB_EXTRA_MS;

        // A cold wireless receiver can miss the host's first look at the port and then sit silent
        // until the port is powered off and on again. Normally the watchdog waits four idle seconds
        // before doing that, which is longer than this wait - so ask for it once, early.
        if (!ready && mounted <= 0 && !nudged && waited >= NUDGE_MS) {
            nudged = true;
            usb_force_cycle = 1;
        }

        char detail[64];
        if (kb || ms) snprintf(detail, sizeof(detail), "%d keyboard%s, %d mouse%s", kb, kb == 1 ? "" : "s",
                               ms, ms == 1 ? "" : "s");
        else if (mounted > 0) snprintf(detail, sizeof(detail), "bus: %d, no pointer %c", mounted,
                                        spin[spin_i++ & 3]);
        else snprintf(detail, sizeof(detail), "no pointer yet %c", spin[spin_i++ & 3]);
        s_stage = BOOT_STAGE_INPUT;
        set_msg(BOOT_STAGE_INPUT, detail);
        set_pct(BOOT_STAGE_INPUT, ready ? cap : (waited > cap ? cap : waited), cap);
        flush_splash();

        if (ready || waited >= cap) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        have_mouse = input_mouse_seen();
    }
    usb_boot_wait = 0;
    ESP_LOGI(TAG, "input settled after %d ms (bus: %d device(s), %s)", (int)(os_uptime_ms() - t0), mounted,
             nudged ? "port cycled" : "port left alone");
}

void boot_done(void)
{
    s_gone_ms = os_uptime_ms();
    s_ready_ms = s_gone_ms;
    ESP_LOGI(TAG, "splash gone at %u ms (input settled: %d keyboard(s), %d mouse(es))",
             (unsigned)s_gone_ms, usb_keyboards(), usb_mice());
}

int boot_elapsed_ms(void) { return s_gone_ms == (uint32_t)-1 ? -1 : (int)(s_gone_ms - s_splash_ms); }
int boot_ready_ms(void) { return (int)s_ready_ms; }
