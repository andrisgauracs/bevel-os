// Compositor: dirty-rectangle tracking, strip rendering in internal RAM, one hand-over to the display.
#pragma once
#include "gfx.h"
#include "display.h"

// Paints everything that intersects g's clip (world coordinates). Called once per strip.
typedef void (*comp_paint_fn)(gfx_t *g, const rect_t *area);

void comp_init(comp_paint_fn paint);
void comp_invalidate(int x, int y, int w, int h);
void comp_invalidate_rect(const rect_t *r);
void comp_invalidate_all(void);
bool comp_dirty(void);
// Repaint and flush all dirty rectangles. Returns the number of pixels written to the framebuffer.
int comp_flush(void);

typedef struct {
    uint32_t flushes;     // comp_flush() calls that did work
    uint32_t pixels;      // pixels sent to the display
    uint32_t render_us;   // time spent painting strips (internal RAM)
    uint32_t copy_us;     // time spent copying and converting strips for the panel
    uint32_t wait_us;     // time spent waiting for the scan-out (VGA board; 0 on the DSI panel)
    uint32_t max_flush_us;// longest single comp_flush()
} comp_stats_t;
void comp_get_stats(comp_stats_t *out, bool reset);

#if DISP_HAS_SCANOUT
// Tuning knobs for the VGA board's benchmarks.
extern int comp_beam_race;   // 1 = wait for the scan-out (tear-free), 0 = write immediately
#endif
