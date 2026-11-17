// Camera: the OV02C10 on the MIPI-CSI connector, live in a window, and "Take picture" saves the whole
// frame as a 24-bit BMP the Picture Viewer can open.
//
// esp_video does the work up to finished pictures: the sensor sends RAW10 at 1288x728, 30 fps, the
// ISP turns that into RGB565 and its pipeline controller runs auto exposure and white balance. The
// frames land in three buffers that the driver fills in turn. A task on the system core takes each
// one, shrinks it by half into a true-colour overlay (the window only paints the overlay's key colour
// where the picture goes), and hands the buffer back. A picture is copied out of a buffer first and
// written from the copy, so the driver never waits on the drive.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "board.h"
#include "display.h"
#include "fsutil.h"

static const char *TAG = "camera";

#define NBUF   3
#define VIEW_W 644      // half of 1288x728
#define VIEW_H 364

typedef struct {
    window_t *win;
    widget_t *view, *status, *mirror;
    int fd, fw, fh, ov, timer;
    uint8_t *buf[NBUF];
    TaskHandle_t task;
    SemaphoreHandle_t done;
    volatile bool stop, want_shot, mirror_on, saving;
    volatile uint32_t frames;
    uint32_t frames_seen;
    uint16_t *shot;                 // one whole frame, copied out for saving
    int16_t col[VIEW_W];            // preview column -> frame column (mirror included)
    char error[96];                 // why there is no picture
} cam_t;

static cam_t *s_cam;                // one Camera window at a time
static bool s_video_ready;
static char s_saved_msg[160];       // written by the camera task, shown by the GUI task

// ---------------------------------------------------------------- picture file

// 24-bit BMP, rows bottom-up, written in strips so the drive gets large writes.
static bool write_bmp(const char *path, const uint16_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int row = w * 3, pad = (4 - row % 4) % 4, stride = row + pad;
    uint32_t size = 54 + (uint32_t)stride * h;
    uint8_t hdr[54] = {'B', 'M'};
    #define PUT32(o, v) do { uint32_t _v = (v); memcpy(hdr + (o), &_v, 4); } while (0)
    PUT32(2, size); PUT32(10, 54); PUT32(14, 40); PUT32(18, w); PUT32(22, h);
    hdr[26] = 1; hdr[28] = 24; PUT32(34, size - 54); PUT32(38, 2835); PUT32(42, 2835);
    #undef PUT32
    bool ok = fwrite(hdr, 1, 54, f) == 54;
    enum { STRIP = 16 };
    uint8_t *strip = heap_caps_malloc((size_t)stride * STRIP, MALLOC_CAP_SPIRAM);
    if (!strip) { fclose(f); return false; }
    for (int y0 = h - 1; ok && y0 >= 0; y0 -= STRIP) {
        int n = y0 + 1 < STRIP ? y0 + 1 : STRIP;
        for (int r = 0; r < n; r++) {
            const uint16_t *s = px + (size_t)(y0 - r) * w;
            uint8_t *d = strip + (size_t)r * stride;
            for (int x = 0; x < w; x++) {
                uint16_t p = s[x];
                d[3 * x]     = (uint8_t)((p << 3) | ((p >> 2) & 7));          // blue
                d[3 * x + 1] = (uint8_t)(((p >> 3) & 0xFC) | ((p >> 9) & 3)); // green
                d[3 * x + 2] = (uint8_t)(((p >> 8) & 0xF8) | (p >> 13));      // red
            }
            memset(d + row, 0, pad);
        }
        ok = fwrite(strip, 1, (size_t)stride * n, f) == (size_t)stride * n;
    }
    free(strip);
    if (fclose(f) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

// On the SD card if there is one (a picture is 2.7 MB), otherwise on the drive; named by the clock.
static void save_picture(cam_t *c)
{
    char dir[64], path[128];
    bool sd = storage_sd_ok() && fs_is_dir(SD_ROOT);
    snprintf(dir, sizeof(dir), "%s/Pictures", sd ? SD_ROOT : DISK_ROOT);
    if (!fs_is_dir(dir)) mkdir(dir, 0777);
    uint64_t total = 0, free_b = 0;
    uint32_t need = 54 + (uint32_t)c->fw * c->fh * 3 + 65536;
    if (storage_df_path(dir, &total, &free_b) && free_b < need) {
        snprintf(s_saved_msg, sizeof(s_saved_msg), "Not saved: %s is full.", sd ? "the SD card" : "the drive");
        return;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2024) {
        snprintf(path, sizeof(path), "%s/Photo %04d-%02d-%02d %02d.%02d.%02d.bmp", dir,
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    }
    for (int i = 1; t.tm_year + 1900 < 2024 || fs_exists(path); i++) {   // no clock yet, or two in a second
        snprintf(path, sizeof(path), "%s/Photo %04d.bmp", dir, i);
        if (!fs_exists(path)) break;
    }
    uint32_t t0 = os_uptime_ms();
    if (write_bmp(path, c->shot, c->fw, c->fh)) {
        snprintf(s_saved_msg, sizeof(s_saved_msg), "Saved %s", path + strlen(DISK_ROOT));
        ESP_LOGI(TAG, "saved %s in %u ms", path, (unsigned)(os_uptime_ms() - t0));
    } else {
        snprintf(s_saved_msg, sizeof(s_saved_msg), "Could not save the picture.");
    }
}

static void saved_gui(void *arg)
{
    if (!s_cam) return;
    s_cam->saving = false;
    ui_set_text(s_cam->status, s_saved_msg);
}

// ---------------------------------------------------------------- capture task

static void set_columns(cam_t *c)
{
    for (int x = 0; x < VIEW_W; x++) {
        int sx = x * c->fw / VIEW_W;
        c->col[x] = c->mirror_on ? c->fw - 1 - sx : sx;
    }
}

static void cam_task(void *arg)
{
    cam_t *c = arg;
    int ow = 0, oh = 0;
    uint16_t *ov = disp_overlay_px(c->ov, &ow, &oh);
    bool mirrored = !c->mirror_on;
    while (!c->stop) {
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (ioctl(c->fd, VIDIOC_DQBUF, &b) != 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        const uint16_t *src = (const uint16_t *)c->buf[b.index];
        if (mirrored != c->mirror_on) { mirrored = c->mirror_on; set_columns(c); }
        if (ov) {
            for (int y = 0; y < VIEW_H && y < oh; y++) {
                const uint16_t *s = src + (size_t)(y * c->fh / VIEW_H) * c->fw;
                uint16_t *d = ov + (size_t)y * ow;
                for (int x = 0; x < VIEW_W; x++) d[x] = s[c->col[x]];
            }
            disp_overlay_update(c->ov);
        }
        bool shoot = c->want_shot;
        if (shoot) {
            if (!c->mirror_on) memcpy(c->shot, src, (size_t)c->fw * c->fh * 2);
            else for (int y = 0; y < c->fh; y++) {
                const uint16_t *s = src + (size_t)y * c->fw;
                uint16_t *d = c->shot + (size_t)y * c->fw;
                for (int x = 0; x < c->fw; x++) d[x] = s[c->fw - 1 - x];
            }
            c->want_shot = false;
        }
        ioctl(c->fd, VIDIOC_QBUF, &b);
        c->frames++;
        if (shoot) { save_picture(c); ev_call(saved_gui, NULL); }
    }
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- starting and stopping

static bool video_ready(char *err, int n)
{
    if (s_video_ready) return true;
    // the sensor shares the I2C bus with touch, the clock and the codec; the display driver already
    // powers the MIPI PHY from the internal LDO, so esp_video leaves the LDO alone
    esp_video_init_csi_config_t csi = {
        .sccb_config = {.init_sccb = false, .i2c_handle = board_i2c(), .freq = 100000},
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dont_init_ldo = true,
    };
    esp_video_init_config_t cfg = {.csi = &csi};
    esp_err_t e = esp_video_init(&cfg);
    if (e != ESP_OK) { snprintf(err, n, "No camera found (%s).", esp_err_to_name(e)); return false; }
    s_video_ready = true;
    return true;
}

static bool cam_start(cam_t *c)
{
    if (!video_ready(c->error, sizeof(c->error))) return false;
    c->fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (c->fd < 0) { strcpy(c->error, "The camera could not be opened."); return false; }
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (ioctl(c->fd, VIDIOC_G_FMT, &fmt) != 0) { strcpy(c->error, "The camera did not report its format."); return false; }
    c->fw = fmt.fmt.pix.width;
    c->fh = fmt.fmt.pix.height;
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format want = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
        want.fmt.pix.width = c->fw;
        want.fmt.pix.height = c->fh;
        want.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(c->fd, VIDIOC_S_FMT, &want) != 0) { strcpy(c->error, "The camera cannot deliver RGB565."); return false; }
    }
    struct v4l2_requestbuffers req = {.count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(c->fd, VIDIOC_REQBUFS, &req) != 0) { strcpy(c->error, "No memory for the camera buffers."); return false; }
    for (int i = 0; i < NBUF; i++) {
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        if (ioctl(c->fd, VIDIOC_QUERYBUF, &b) != 0) { strcpy(c->error, "The camera buffers could not be set up."); return false; }
        c->buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, b.m.offset);
        if (!c->buf[i] || ioctl(c->fd, VIDIOC_QBUF, &b) != 0) { strcpy(c->error, "The camera buffers could not be set up."); return false; }
    }
    c->shot = heap_caps_malloc((size_t)c->fw * c->fh * 2, MALLOC_CAP_SPIRAM);
    c->ov = disp_overlay_alloc(VIEW_W, VIEW_H);
    if (!c->shot || c->ov < 0) { strcpy(c->error, "Not enough memory for the picture."); return false; }
    set_columns(c);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(c->fd, VIDIOC_STREAMON, &type) != 0) { strcpy(c->error, "The camera did not start."); return false; }
    // esp_video's ISP pipeline turns its own tag up to debug whenever the build allows it, and the
    // sensor driver logs every exposure change: several lines a frame would bury the serial console
    esp_log_level_set("ISP", ESP_LOG_WARN);
    esp_log_level_set("ov02c10", ESP_LOG_WARN);
    c->done = xSemaphoreCreateBinary();
    if (xTaskCreatePinnedToCore(cam_task, "camera", 4096, c, 4, &c->task, SYS_CORE) != pdPASS) {
        strcpy(c->error, "The camera task could not start.");
        c->task = NULL;
        return false;
    }
    ESP_LOGI(TAG, "streaming %dx%d", c->fw, c->fh);
    return true;
}

static void cam_stop(cam_t *c)
{
    if (c->task) {
        c->stop = true;
        // a picture being written finishes first; a dead stream gives up after a few seconds
        if (xSemaphoreTake(c->done, pdMS_TO_TICKS(c->saving ? 15000 : 3000)) != pdTRUE) ESP_LOGW(TAG, "capture task did not stop");
    }
    if (c->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(c->fd, VIDIOC_STREAMOFF, &type);
        close(c->fd);
    }
    if (c->ov >= 0) disp_overlay_free(c->ov);
    if (c->done) vSemaphoreDelete(c->done);
    free(c->shot);
}

// ---------------------------------------------------------------- window

static void view_paint(widget_t *w, gfx_t *g)
{
    cam_t *c = w->win->user;
    if (c->ov >= 0 && c->task) {
        disp_overlay_place(c->ov, g->ox, g->oy);
        gfx_fill(g, 0, 0, w->r.w, w->r.h, DISP_OVERLAY_KEY(c->ov));
        return;
    }
    gfx_fill(g, 0, 0, w->r.w, w->r.h, C_BLACK);
    const char *m = c->error[0] ? c->error : "Starting the camera...";
    gfx_text(g, (w->r.w - gfx_text_w(m, -1, FONT_UI)) / 2, w->r.h / 2 - 6, m, -1, FONT_UI, C_WHITE);
}
static const canvas_ops_t view_ops = {view_paint, NULL, NULL};

static void take_picture(widget_t *b, int code)
{
    cam_t *c = b->win->user;
    if (!c->task || c->saving) return;
    c->saving = true;
    ui_set_text(c->status, "Saving...");
    c->want_shot = true;
}

static void mirror_changed(widget_t *b, int code)
{
    cam_t *c = b->win->user;
    c->mirror_on = ui_checked(b);
}

static void tick(void *arg)
{
    cam_t *c = arg;
    if (s_cam != c || c->saving) return;
    uint32_t n = c->frames, fps = n - c->frames_seen;
    c->frames_seen = n;
    if (s_saved_msg[0]) return;     // keep showing where the last picture went
    char t[64];
    snprintf(t, sizeof(t), "%d x %d, %u frames per second", c->fw, c->fh, (unsigned)fps);
    ui_set_text(c->status, t);
}

static void cam_destroy(window_t *w)
{
    cam_t *c = w->user;
    if (c->timer) timer_del(c->timer);
    s_cam = NULL;
    cam_stop(c);
    free(c);
}

void app_camera(const char *arg)
{
    if (s_cam) { wm_activate(s_cam->win); return; }
    cam_t *c = heap_caps_calloc(1, sizeof(cam_t), MALLOC_CAP_INTERNAL);
    if (!c) return;
    c->fd = -1; c->ov = -1;
    s_saved_msg[0] = 0;
    window_t *w = wm_create("Camera", VIEW_W + 16, VIEW_H + 52, 0);
    w->icon = &icon16_camera;
    w->user = c;
    w->on_destroy = cam_destroy;
    c->win = w;
    s_cam = c;
    c->view = ui_canvas(w, 8, 8, VIEW_W, VIEW_H, &view_ops, false);
    widget_t *shot = ui_button(w, 8, VIEW_H + 18, 110, 26, "Take &picture", take_picture);
    c->mirror = ui_checkbox(w, 130, VIEW_H + 23, 80, "&Mirror", false);
    c->mirror->on_action = mirror_changed;
    c->status = ui_label(w, 220, VIEW_H + 24, VIEW_W - 212, 14, "");
    w->def_btn = shot;
    w->focus = shot;
    wm_show(w);
    wm_pump(0);                     // the window is up while the sensor is found and started
    wm_busy(true);
    bool ok = cam_start(c);
    wm_busy(false);
    if (!ok) {
        ESP_LOGW(TAG, "%s", c->error);
        cam_stop(c);
        c->fd = -1; c->ov = -1; c->task = NULL; c->done = NULL; c->shot = NULL;
        ui_enable(shot, false);
        ui_enable(c->mirror, false);
    } else {
        c->timer = timer_add(1000, true, tick, c);
    }
    ui_invalidate(c->view);
}
