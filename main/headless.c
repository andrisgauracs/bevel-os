// Headless display backend: the framebuffer without a local screen. The LCD peripheral is never
// started, so the VGA pins are free and no CPU time or PSRAM bandwidth goes to the scan-out.
// The desktop is seen through the VNC server, or through the SDL window of the simulator.
#include <string.h>
#include "vga.h"
#include "esp_heap_caps.h"

static uint8_t *s_fb;
static int s_copy_mode;

esp_err_t vga_init(void)
{
    if (!s_fb) s_fb = heap_caps_aligned_calloc(64, 1, VGA_W * VGA_H, MALLOC_CAP_SPIRAM);
    return s_fb ? ESP_OK : ESP_ERR_NO_MEM;
}

uint8_t *vga_fb(void) { return s_fb; }

const char *vga_name(void)
{
#if CONFIG_BEVEL_VNC
    return "640 x 480, 64 colours, remote (VNC)";
#else
    return "640 x 480, 64 colours, no screen";
#endif
}

// Nothing reads the framebuffer on a schedule, so writes never need to wait.
int vga_wait_rows(int y0, int y1, int est_us) { (void)y0; (void)y1; (void)est_us; return 0; }
int vga_fetch_row(void) { return 0; }

void vga_get_stats(vga_stats_t *out, bool reset) { (void)reset; memset(out, 0, sizeof(*out)); }

void vga_set_copy_mode(int mode) { s_copy_mode = mode; }
int vga_get_copy_mode(void) { return s_copy_mode; }
