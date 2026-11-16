// Benchmarks ("perf" serial command, or the Benchmark app).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_async_memcpy.h"
#include "esp_attr.h"
#include "os.h"
#include "display.h"
#include "comp.h"
#include "wm.h"
#include "apps.h"

perf_result_t perf_results[PERF_MAX];
int perf_count;

uint32_t os_uptime_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void report(const char *name, uint32_t value, const char *unit)
{
    serial_printf("PERF %-26s %8u %-8s\n", name, (unsigned)value, unit);
    int i;
    for (i = 0; i < perf_count; i++) if (!strcmp(perf_results[i].name, name)) break;
    if (i == PERF_MAX) return;
    strlcpy(perf_results[i].name, name, sizeof(perf_results[i].name));
    perf_results[i].value = value;
    perf_results[i].unit = unit;
    if (i == perf_count) perf_count++;
}

static inline int64_t now_us(void) { return esp_timer_get_time(); }
static void begin(void) { }
static uint32_t kb_per_s(uint64_t bytes, int64_t us) { return (uint32_t)(bytes * 1000000ull / 1024 / (us ? us : 1)); }

__attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
static void IRAM_ATTR fill_bytes(uint8_t *d, uint8_t c, int n)
{
    for (int i = 0; i < n; i++) d[i] = c;
}

__attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
static void IRAM_ATTR copy_bytes(uint8_t *d, const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static bool IRAM_ATTR dma_done(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &hp);
    return hp == pdTRUE;
}

#define INT_N (16 * 1024)
#define EXT_N (256 * 1024)

static void bench_mem(void)
{
    uint8_t *ib = heap_caps_aligned_alloc(16, INT_N, MALLOC_CAP_INTERNAL);
    uint8_t *ib2 = heap_caps_aligned_alloc(16, INT_N, MALLOC_CAP_INTERNAL);
    uint8_t *eb = heap_caps_aligned_alloc(64, EXT_N, MALLOC_CAP_SPIRAM);
    if (!ib || !ib2 || !eb) { serial_printf("perf: no memory\n"); goto done; }
    int64_t t;
    const int R = 400;

    begin(); t = now_us();
    for (int i = 0; i < R / 8; i++) fill_bytes(ib, i, INT_N);
    report("fill int byte-loop", kb_per_s((uint64_t)INT_N * (R / 8), now_us() - t), "KB/s");
    begin(); t = now_us();
    for (int i = 0; i < R; i++) memset(ib, i, INT_N);
    report("fill int memset", kb_per_s((uint64_t)INT_N * R, now_us() - t), "KB/s");

    begin(); t = now_us();
    for (int i = 0; i < R / 8; i++) copy_bytes(ib2, ib, INT_N);
    report("copy int>int byte-loop", kb_per_s((uint64_t)INT_N * (R / 8), now_us() - t), "KB/s");
    begin(); t = now_us();
    for (int i = 0; i < R; i++) memcpy(ib2, ib, INT_N);
    report("copy int>int memcpy", kb_per_s((uint64_t)INT_N * R, now_us() - t), "KB/s");

    const int E = 8;
    begin(); t = now_us();
    for (int i = 0; i < E; i++) memset(eb, i, EXT_N);
    report("fill PSRAM memset", kb_per_s((uint64_t)EXT_N * E, now_us() - t), "KB/s");

    begin(); t = now_us();
    for (int i = 0; i < E; i++) for (int o = 0; o < EXT_N; o += INT_N) memcpy(eb + o, ib, INT_N);
    report("copy int>PSRAM memcpy", kb_per_s((uint64_t)EXT_N * E, now_us() - t), "KB/s");
    begin(); t = now_us();
    for (int i = 0; i < E; i++) for (int o = 0; o < EXT_N; o += INT_N) memcpy(ib, eb + o, INT_N);
    report("copy PSRAM>int memcpy", kb_per_s((uint64_t)EXT_N * E, now_us() - t), "KB/s");

    // GDMA copy (the CPU is free while it runs; here we just wait for it)
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    async_memcpy_handle_t h = NULL;
    if (esp_async_memcpy_install(&cfg, &h) == ESP_OK) {
        SemaphoreHandle_t sem = xSemaphoreCreateBinary();
        bool ok = true;
        begin(); t = now_us();
        for (int i = 0; i < E && ok; i++) {
            for (int o = 0; o < EXT_N && ok; o += INT_N) {
                if (esp_async_memcpy(h, eb + o, ib, INT_N, dma_done, sem) != ESP_OK) ok = false;
                else xSemaphoreTake(sem, pdMS_TO_TICKS(1000));
            }
        }
        if (ok) report("copy int>PSRAM GDMA", kb_per_s((uint64_t)EXT_N * E, now_us() - t), "KB/s");
        else serial_printf("perf: GDMA int>PSRAM copy rejected\n");
        ok = true;
        begin(); t = now_us();
        for (int i = 0; i < E && ok; i++) {
            for (int o = 0; o < EXT_N && ok; o += INT_N) {
                if (esp_async_memcpy(h, ib, eb + o, INT_N, dma_done, sem) != ESP_OK) ok = false;
                else xSemaphoreTake(sem, pdMS_TO_TICKS(1000));
            }
        }
        if (ok) report("copy PSRAM>int GDMA", kb_per_s((uint64_t)EXT_N * E, now_us() - t), "KB/s");
        else serial_printf("perf: GDMA PSRAM>int copy rejected\n");
        esp_async_memcpy_uninstall(h);
        vSemaphoreDelete(sem);
    }
done:
    free(ib); free(ib2); free(eb);
}

static void bench_draw(void)
{
    const int W = 640, H = 24;
    uint8_t *ib = heap_caps_aligned_alloc(16, W * H, MALLOC_CAP_INTERNAL);
    if (!ib) return;
    gfx_t g;
    gfx_init(&g, ib, W, 0, 0, W, H);
    static const char text[] = "The quick brown fox jumps over the lazy dog 0123456789";
    const int len = sizeof(text) - 1;
    int64_t t;
    const int R = 3000;

    begin(); t = now_us();
    for (int i = 0; i < R; i++) gfx_text(&g, 2, 2, text, len, FONT_UI, C_BLACK);
    report("text UI font", (uint32_t)((uint64_t)R * len * 1000000 / (now_us() - t)), "glyph/s");
    begin(); t = now_us();
    for (int i = 0; i < R; i++) gfx_text(&g, 2, 2, text, len, FONT_UI_BOLD, C_BLACK);
    report("text UI bold", (uint32_t)((uint64_t)R * len * 1000000 / (now_us() - t)), "glyph/s");
    begin(); t = now_us();
    for (int i = 0; i < R; i++) gfx_text(&g, 2, 2, text, len, FONT_MONO, C_BLACK);
    report("text mono 8x16", (uint32_t)((uint64_t)R * len * 1000000 / (now_us() - t)), "glyph/s");

    begin(); t = now_us();
    for (int i = 0; i < R * 10; i++) gfx_fill(&g, 10 + (i & 63), 2, 100, 20, (uint8_t)i);
    report("fill rect 100x20", (uint32_t)((uint64_t)R * 10 * 1000000 / (now_us() - t)), "rect/s");
    begin(); t = now_us();
    for (int i = 0; i < R * 10; i++) {
        int x = 10 + (i & 63);
        gfx_fill(&g, x, 1, 75, 22, C_GREY);
        gfx_bevel(&g, x, 1, 75, 22, C_WHITE, C_BLACK);
        gfx_bevel(&g, x + 1, 2, 73, 20, C_GREY, C_DKGREY);
    }
    report("bevel button 75x22", (uint32_t)((uint64_t)R * 10 * 1000000 / (now_us() - t)), "btn/s");
    begin(); t = now_us();
    for (int i = 0; i < R * 10; i++) gfx_bitmap(&g, 10 + (i & 255), -4, &icon32_folder);
    report("icon 32x32 keyed", (uint32_t)((uint64_t)R * 10 * 1000000 / (now_us() - t)), "icon/s");
    free(ib);
}

static void bench_compose(void)
{
    const int N = 30;
    comp_stats_t c;
    comp_get_stats(&c, true);
    int64_t t = now_us();
    for (int i = 0; i < N; i++) { comp_invalidate_all(); comp_flush(); }
    int64_t dt = now_us() - t;
    comp_get_stats(&c, true);
    report("full redraw", (uint32_t)(N * 10000000ull / dt), "FPS x10");
    report("  render per frame", c.render_us / N, "us");
    report("  convert per frame", c.copy_us / N, "us");
    // a window-sized update
    t = now_us();
    for (int i = 0; i < N * 2; i++) { comp_invalidate(120 + (i & 7) * 8, 90, 320, 240); comp_flush(); }
    dt = now_us() - t;
    report("redraw 320x240", (uint32_t)(N * 2 * 10000000ull / dt), "FPS x10");
    t = now_us();
    for (int i = 0; i < 2000; i++) { comp_invalidate(300 + (i & 63), 200, 16, 16); comp_flush(); }
    dt = now_us() - t;
    report("redraw 16x16", (uint32_t)(2000 * 1000000ull / dt), "upd/s");
}

// Drag a real window (Help: a 488x357 list full of text) around the desktop, one step per frame.
static void bench_drag(void)
{
    app_help(NULL);
    window_t *w = wm_active();
    if (!w) return;
    wm_pump(0);
    static const int8_t path[16][2] = {{8,0},{8,3},{6,6},{3,8},{0,8},{-3,8},{-6,6},{-8,3},{-8,0},{-8,-3},{-6,-6},{-3,-8},{0,-8},{3,-8},{6,-6},{8,-3}};
    const int N = 160;
    int x = 60, y = 30;
    wm_move(w, x, y);
    comp_flush();
    comp_stats_t c;
    comp_get_stats(&c, true);
    begin();
    int64_t t = now_us();
    for (int i = 0; i < N; i++) {
        x += path[i & 15][0]; y += path[i & 15][1];
        wm_move(w, x, y);
        comp_flush();
    }
    int64_t dt = now_us() - t;
    comp_get_stats(&c, true);
    report("window drag 488x357", (uint32_t)(N * 10000000ull / dt), "FPS x10");
    report("  drag: render per frame", c.render_us / N, "us");
    report("  drag: convert per frame", c.copy_us / N, "us");
    report("  drag: pixels per frame", c.pixels / N, "px");
    wm_destroy(w);
    wm_pump(0);
}

void perf_run(const char *which)
{
    bool all = !which || !*which || !strcmp(which, "all");
    if (all || !strcmp(which, "mem")) bench_mem();
    if (all || !strcmp(which, "draw")) bench_draw();
    if (all || !strcmp(which, "compose")) bench_compose();
    if (all || !strcmp(which, "drag")) bench_drag();
    report("free internal RAM", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, "KB");
    report("largest internal block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024, "KB");
    report("free PSRAM", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024, "KB");
    comp_invalidate_all();
}
