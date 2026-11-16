// Compositor.
//
// Nothing ever draws straight into the PSRAM framebuffer. Damage is collected as a short list of
// rectangles; each one is painted strip by strip into a 20 KB scratch buffer in internal RAM
// (fast, and overdraw there is cheap), then copied to PSRAM exactly once, timed so the scan-out
// is not reading those rows ("beam racing"). That gives flicker-free and tear-free updates with a
// single framebuffer and the minimum possible PSRAM traffic.
#include <string.h>
#include "comp.h"
#include "vga.h"
#include "esp_attr.h"
#include "esp_cpu.h"

#define MAX_DIRTY 12
#define SCRATCH_BYTES (VGA_W * 32)
#define CPU_MHZ 240

static DRAM_ATTR __attribute__((aligned(16))) uint8_t s_scratch[SCRATCH_BYTES];
static DRAM_ATTR rect_t s_dirty[MAX_DIRTY];
static DRAM_ATTR int s_ndirty;
static comp_paint_fn s_paint;
static comp_stats_t s_stats;
static uint32_t s_ns_per_px = 40; // moving estimate of render+copy cost, for the beam race

int comp_beam_race = 1;
int comp_copy_mode = 1;

void comp_init(comp_paint_fn paint) { s_paint = paint; s_ndirty = 0; }

static inline int area(const rect_t *r) { return r->w * r->h; }

void IRAM_ATTR comp_invalidate_rect(const rect_t *in)
{
    rect_t r, screen = {0, 0, VGA_W, VGA_H};
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

void comp_invalidate_all(void) { comp_invalidate(0, 0, VGA_W, VGA_H); }
bool comp_dirty(void) { return s_ndirty > 0; }

int IRAM_ATTR comp_flush(void)
{
    if (!s_ndirty) return 0;
    uint32_t t_start = esp_cpu_get_cycle_count();
    uint32_t c_render = 0, c_copy = 0, us_wait = 0;
    int total_px = 0;
    uint8_t *fb = vga_fb();

    // top to bottom, so a multi-rectangle update follows the beam
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
        // widen to a 16-pixel grid: aligned 128-bit stores and whole cache lines
        int x0 = list[i].x & ~15;
        int x1 = (list[i].x + list[i].w + 15) & ~15;
        if (x1 > VGA_W) x1 = VGA_W;
        int w = x1 - x0;
        int y = list[i].y, y_end = list[i].y + list[i].h;
        int rows_max = SCRATCH_BYTES / w;
        bool first = true;
        while (y < y_end) {
            int rows = y_end - y < rows_max ? y_end - y : rows_max;
            rect_t strip = {x0, y, w, rows};
            gfx_t g;
            gfx_init(&g, s_scratch, w, x0, y, w, rows);
            uint32_t t0 = esp_cpu_get_cycle_count();
            s_paint(&g, &strip);
            uint32_t t1 = esp_cpu_get_cycle_count();
            if (comp_beam_race) {
                // First strip: the scan-out must stay clear of it for the whole remaining update, so
                // the beam is ahead of us and we chase it down the screen. Later strips: just never
                // overtake the beam.
                int px = first ? w * (y_end - y) : w * rows;
                us_wait += vga_wait_rows(y, y + rows, (int)((uint64_t)px * s_ns_per_px / 1000) + 20);
            }
            uint32_t t2 = esp_cpu_get_cycle_count();
            uint8_t *d = fb + y * VGA_W + x0;
            const uint8_t *s = s_scratch;
            if (comp_copy_mode == 1) {
                for (int r = 0; r < rows; r++, d += VGA_W, s += w) pie_copy16(d, s, w >> 4);
            } else {
                for (int r = 0; r < rows; r++, d += VGA_W, s += w) memcpy(d, s, w);
            }
            uint32_t t3 = esp_cpu_get_cycle_count();
            c_render += t1 - t0;
            c_copy += t3 - t2;
            total_px += w * rows;
            y += rows;
            first = false;
        }
        display_damage(x0, list[i].y, w, list[i].h);
    }

    uint32_t work = c_render + c_copy;
    if (total_px > 2000) {
        uint32_t ns = (uint32_t)((uint64_t)work * 1000 / CPU_MHZ / total_px);
        s_ns_per_px = (s_ns_per_px * 3 + ns) / 4;
    }
    uint32_t total_us = (esp_cpu_get_cycle_count() - t_start) / CPU_MHZ;
    s_stats.flushes++;
    s_stats.pixels += total_px;
    s_stats.render_us += c_render / CPU_MHZ;
    s_stats.copy_us += c_copy / CPU_MHZ;
    s_stats.wait_us += us_wait;
    if (total_us > s_stats.max_flush_us) s_stats.max_flush_us = total_us;
    return total_px;
}

void comp_get_stats(comp_stats_t *out, bool reset)
{
    *out = s_stats;
    if (reset) memset(&s_stats, 0, sizeof(s_stats));
}
