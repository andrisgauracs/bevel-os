// Compositor.
//
// Nothing ever draws straight into the framebuffer. Damage is collected as a short list of
// rectangles; each one is painted strip by strip into a scratch buffer in internal RAM (fast, and
// overdraw there is cheap), then handed to the display once. On the DSI panel that means converting
// it to RGB565; on the VGA board the strip goes into PSRAM, and before it does the compositor waits
// until the scan-out is not about to read those rows ("beam racing"). Either way: flicker-free
// updates with the minimum possible PSRAM traffic.
#include <string.h>
#include "comp.h"
#include "display.h"
#include "esp_attr.h"
#include "esp_timer.h"

#define MAX_DIRTY 12
#define SCRATCH_BYTES (SCREEN_W * 32)

static DRAM_ATTR __attribute__((aligned(16))) uint8_t s_scratch[SCRATCH_BYTES];
static DRAM_ATTR rect_t s_dirty[MAX_DIRTY];
static DRAM_ATTR int s_ndirty;
static comp_paint_fn s_paint;
static comp_stats_t s_stats;

#if DISP_HAS_SCANOUT
int comp_beam_race = 1;
static uint32_t s_ns_per_px = 40;   // moving estimate of render+copy cost, for the beam race
#endif

void comp_init(comp_paint_fn paint) { s_paint = paint; s_ndirty = 0; }

static inline int area(const rect_t *r) { return r->w * r->h; }

void IRAM_ATTR comp_invalidate_rect(const rect_t *in)
{
    rect_t r, screen = {0, 0, SCREEN_W, SCREEN_H};
    if (!rect_intersect(&r, in, &screen)) return;
    for (;;) {
        // Merge with any rectangle where the union wastes little; repeat as the result may now touch others.
        int merged = -1;
        for (int i = 0; i < s_ndirty; i++) {
            rect_t u;
            rect_union(&u, &r, &s_dirty[i]);
            if (area(&u) <= area(&r) + area(&s_dirty[i]) + 256) { r = u; merged = i; break; }
        }
        if (merged < 0) break;
        s_dirty[merged] = s_dirty[--s_ndirty];
    }
    if (s_ndirty == MAX_DIRTY) {
        // Full: fold the new one into whichever existing rectangle grows the least.
        int best = 0, best_cost = INT32_MAX;
        for (int i = 0; i < s_ndirty; i++) {
            rect_t u;
            rect_union(&u, &r, &s_dirty[i]);
            int cost = area(&u) - area(&s_dirty[i]);
            if (cost < best_cost) { best_cost = cost; best = i; }
        }
        rect_union(&s_dirty[best], &s_dirty[best], &r);
        return;
    }
    s_dirty[s_ndirty++] = r;
}

void comp_invalidate(int x, int y, int w, int h)
{
    rect_t r = {x, y, w, h};
    comp_invalidate_rect(&r);
}

void comp_invalidate_all(void) { comp_invalidate(0, 0, SCREEN_W, SCREEN_H); }
bool comp_dirty(void) { return s_ndirty > 0; }

int IRAM_ATTR comp_flush(void)
{
    if (!s_ndirty) return 0;
    int64_t t_start = esp_timer_get_time();
    uint32_t us_render = 0, us_copy = 0;
    int total_px = 0;
#if DISP_HAS_SCANOUT
    uint32_t us_wait = 0;
#endif

    // top to bottom, so a multi-rectangle update appears in reading order
    for (int i = 1; i < s_ndirty; i++) {
        rect_t t = s_dirty[i];
        int j = i;
        for (; j > 0 && s_dirty[j - 1].y > t.y; j--) s_dirty[j] = s_dirty[j - 1];
        s_dirty[j] = t;
    }
    int n = s_ndirty;
    rect_t list[MAX_DIRTY];
    memcpy(list, s_dirty, n * sizeof(rect_t));
    s_ndirty = 0; // painting may invalidate again (it should not, but must not corrupt the list)

    for (int i = 0; i < n; i++) {
        int x0 = list[i].x, w = list[i].w;
#if DISP_HAS_SCANOUT
        // widen to a 16-pixel grid: aligned 128-bit stores and whole cache lines
        x0 = list[i].x & ~15;
        int x1 = (list[i].x + list[i].w + 15) & ~15;
        if (x1 > SCREEN_W) x1 = SCREEN_W;
        w = x1 - x0;
#endif
        int y = list[i].y, y_end = list[i].y + list[i].h;
        int rows_max = SCRATCH_BYTES / w;
#if DISP_HAS_SCANOUT
        bool first = true;    // the beam race treats the first strip of an update differently
#endif
        while (y < y_end) {
            int rows = y_end - y < rows_max ? y_end - y : rows_max;
            rect_t strip = {x0, y, w, rows};
            gfx_t g;
            gfx_init(&g, s_scratch, w, x0, y, w, rows);
            int64_t t0 = esp_timer_get_time();
            s_paint(&g, &strip);
            int64_t t1 = esp_timer_get_time();
#if DISP_HAS_SCANOUT
            if (comp_beam_race) {
                // First strip: the scan-out must stay clear of it for the whole remaining update, so
                // the beam is ahead of us and we chase it down the screen. Later strips: just never
                // overtake the beam.
                int px = first ? w * (y_end - y) : w * rows;
                us_wait += disp_wait_rows(y, y + rows, (int)((uint64_t)px * s_ns_per_px / 1000) + 20);
            }
#endif
            disp_put(x0, y, w, rows, s_scratch, w);
            us_render += (uint32_t)(t1 - t0);
            us_copy += (uint32_t)(esp_timer_get_time() - t1);
            total_px += w * rows;
            y += rows;
#if DISP_HAS_SCANOUT
            first = false;
#endif
        }
    }

#if DISP_HAS_SCANOUT
    // The better the estimate of what a pixel costs, the less the beam race waits for nothing.
    if (total_px > 2000) {
        uint32_t ns = (uint32_t)((uint64_t)(us_render + us_copy) * 1000 / total_px);
        s_ns_per_px = (s_ns_per_px * 3 + ns) / 4;
    }
    s_stats.wait_us += us_wait;
#endif

    uint32_t total_us = (uint32_t)(esp_timer_get_time() - t_start);
    s_stats.flushes++;
    s_stats.pixels += total_px;
    s_stats.render_us += us_render;
    s_stats.copy_us += us_copy;
    if (total_us > s_stats.max_flush_us) s_stats.max_flush_us = total_us;
    return total_px;
}

void comp_get_stats(comp_stats_t *out, bool reset)
{
    *out = s_stats;
    if (reset) memset(&s_stats, 0, sizeof(s_stats));
}
