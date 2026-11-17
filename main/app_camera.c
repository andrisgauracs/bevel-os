// Camera: the OV02C10 on the MIPI-CSI connector, live in a window, and "Take picture" saves the whole
// frame as a 24-bit BMP the Picture Viewer can open.
//
// esp_video does the work up to finished pictures: the sensor sends RAW10 in one of its modes
// (1288x728 or 1920x1080, picked in the window), the ISP turns that into RGB565 and its pipeline
// controller runs auto exposure and white balance. The frames land in three buffers that the driver
// fills in turn. A task on the system core takes each one, scales it into a 644x364 true-colour
// overlay (the window only paints the overlay's key colour where the picture goes), and hands the
// buffer back. Both steps are bound by PSRAM bandwidth (the panel scan-out and the camera's DMA use it
// too): about 18 ms to shrink a 1288x728 frame and 18 ms to show it, so the preview runs at about 25
// frames a second. The PPA (the P4's scale/mirror DMA engine) was tried for the shrinking and took
// 42 ms, its driver cleaning the cache over the whole input frame for every job; showing an overlay
// a pixel at a time took 34 ms before display_dsi.c copied its runs whole. A picture is copied out of a buffer first and written from the copy, so the driver
// never waits on the drive.
//
// The capture task runs below everything else on the system core (USB, touch, the web server, the
// drive): the preview takes whatever time is left over, so the camera can make it slower but cannot
// stop the rest of the machine.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "board.h"
#include "display.h"
#include "fsutil.h"

static const char *TAG = "camera";

#define NBUF      3
#define VIEW_W    644     // half of 1288x728
#define VIEW_H    364
#define MAX_MODES 4
#define CAM_PRIO  2       // below the web server (3), USB, touch and the drive

typedef struct {
    window_t *win;
    widget_t *view, *status, *mirror, *res, *shot_btn;
    int fd, fw, fh, ov, timer;
    uint8_t *buf[NBUF];
    TaskHandle_t task;
    SemaphoreHandle_t done;
    volatile bool stop, want_shot, mirror_on, saving;
    volatile uint32_t frames, work_us;
    uint32_t frames_seen;
    uint16_t *shot;                 // one whole frame, copied out for saving
    int px, py, pw, ph;             // where the scaled frame sits in the preview (centred)
    bool half;                      // the frame is exactly twice the preview: a faster path
    int16_t col[VIEW_W];            // preview column -> frame column (mirror included)
    char error[96];                 // why there is no picture
} cam_t;

// the sensor's modes, as esp_video lists them (the same for every window, so kept across them)
static esp_cam_sensor_format_t s_modes[MAX_MODES];
static char s_mode_label[MAX_MODES][24];
static int s_nmodes, s_mode = -1;

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

// On the SD card if there is one (a picture is 2.7 or 6.2 MB), otherwise on the drive; named by the
// clock, or numbered while the clock is not set.
static void save_picture(cam_t *c)
{
    char dir[64], path[128] = "";
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
    if (t.tm_year + 1900 >= 2024)
        snprintf(path, sizeof(path), "%s/Photo %04d-%02d-%02d %02d.%02d.%02d.bmp", dir,
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    for (int i = 1; !path[0] || fs_exists(path); i++) snprintf(path, sizeof(path), "%s/Photo %04d.bmp", dir, i);
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
    for (int x = 0; x < c->pw; x++) {
        int sx = x * c->fw / c->pw;
        c->col[x] = c->mirror_on ? c->fw - 1 - sx : sx;
    }
}

// The frame, shrunk into the preview surface (nearest pixel).
static void scale_preview(cam_t *c, const uint16_t *src, uint16_t *ov, int ow)
{
    if (c->half) {
        // every other pixel of every other row: two pixels per 32-bit read, the even one kept
        for (int y = 0; y < c->ph; y++) {
            const uint32_t *s = (const uint32_t *)(src + (size_t)(2 * y) * c->fw);
            uint16_t *d = ov + (size_t)(c->py + y) * ow + c->px;
            if (!c->mirror_on) for (int x = 0; x < c->pw; x++) d[x] = (uint16_t)s[x];
            else for (int x = 0, k = c->pw - 1; x < c->pw; x++, k--) d[x] = (uint16_t)s[k];
        }
        return;
    }
    for (int y = 0; y < c->ph; y++) {
        const uint16_t *s = src + (size_t)(y * c->fh / c->ph) * c->fw;
        uint16_t *d = ov + (size_t)(c->py + y) * ow + c->px;
        for (int x = 0; x < c->pw; x++) d[x] = s[c->col[x]];
    }
}

static void cam_task(void *arg)
{
    cam_t *c = arg;
    int ow = 0, oh = 0;
    uint16_t *ov = disp_overlay_px(c->ov, &ow, &oh);
    bool mirrored = c->mirror_on;
    uint32_t work = 0, n = 0;
    while (!c->stop) {
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (ioctl(c->fd, VIDIOC_DQBUF, &b) != 0) continue;     // times out after 500 ms: look at stop again
        int64_t t0 = esp_timer_get_time();
        const uint16_t *src = (const uint16_t *)c->buf[b.index];
        if (mirrored != c->mirror_on) { mirrored = c->mirror_on; set_columns(c); }
        if (ov) {
            scale_preview(c, src, ov, ow);
            disp_overlay_update(c->ov);
        }
        bool shoot = c->want_shot;
        if (shoot) {
            if (!mirrored) memcpy(c->shot, src, (size_t)c->fw * c->fh * 2);
            else for (int y = 0; y < c->fh; y++) {
                const uint16_t *s = src + (size_t)y * c->fw;
                uint16_t *d = c->shot + (size_t)y * c->fw;
                for (int x = 0; x < c->fw; x++) d[x] = s[c->fw - 1 - x];
            }
            c->want_shot = false;
        }
        ioctl(c->fd, VIDIOC_QBUF, &b);
        c->frames++;
        work += (uint32_t)(esp_timer_get_time() - t0);
        if (++n == 30) { c->work_us = work / n; work = n = 0; }
        if (shoot) { save_picture(c); ev_call(saved_gui, NULL); }
        // a frame costs a little more than a frame's time, so without a pause this task would never
        // wait for the next one and nothing else at its priority (nor the idle task) would run
        vTaskDelay(pdMS_TO_TICKS(3));
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

// The sensor's modes, once: "1288 x 728", "1920 x 1080", and which one it is in now.
static void list_modes(int fd)
{
    if (s_nmodes) return;
    for (int i = 0; s_nmodes < MAX_MODES; i++) {
        struct v4l2_sensor_format_enum e = {.index = i};
        if (ioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &e) != 0) break;
        s_modes[s_nmodes] = e.format;
        snprintf(s_mode_label[s_nmodes], sizeof(s_mode_label[0]), "%u x %u", e.format.width, e.format.height);
        s_nmodes++;
    }
    esp_cam_sensor_format_t cur = {0};
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &cur) == 0)
        for (int i = 0; i < s_nmodes; i++) if (s_modes[i].width == cur.width && s_modes[i].height == cur.height) s_mode = i;
    if (s_mode < 0 && s_nmodes) s_mode = 0;
}

static bool cam_start(cam_t *c)
{
    c->stop = false;
    c->error[0] = 0;
    if (!video_ready(c->error, sizeof(c->error))) return false;
    c->fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (c->fd < 0) { strcpy(c->error, "The camera could not be opened."); return false; }
    list_modes(c->fd);
    if (s_mode >= 0) {
        esp_cam_sensor_format_t cur = {0};
        ioctl(c->fd, VIDIOC_G_SENSOR_FMT, &cur);
        if (cur.width != s_modes[s_mode].width || cur.height != s_modes[s_mode].height) {
            if (ioctl(c->fd, VIDIOC_S_SENSOR_FMT, &s_modes[s_mode]) != 0) { strcpy(c->error, "The camera refused that resolution."); return false; }
        }
    }
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
    // a frame that never comes must not hold the task for ever: closing the window waits for it
    struct timeval to = {.tv_sec = 0, .tv_usec = 500000};
    ioctl(c->fd, VIDIOC_S_DQBUF_TIMEOUT, &to);
    c->shot = heap_caps_malloc((size_t)c->fw * c->fh * 2, MALLOC_CAP_SPIRAM);
    if (c->ov < 0) c->ov = disp_overlay_alloc(VIEW_W, VIEW_H);
    if (!c->shot || c->ov < 0) { strcpy(c->error, "Not enough memory for the picture."); return false; }
    // the whole frame, as large as fits the preview and centred; the surface is cleared first, so a
    // mode with another shape gets black borders
    c->half = c->fw == 2 * VIEW_W && c->fh == 2 * VIEW_H;
    if ((long)VIEW_W * c->fh <= (long)VIEW_H * c->fw) { c->pw = VIEW_W; c->ph = VIEW_W * c->fh / c->fw; }
    else { c->ph = VIEW_H; c->pw = VIEW_H * c->fw / c->fh; }
    c->px = (VIEW_W - c->pw) / 2;
    c->py = (VIEW_H - c->ph) / 2;
    set_columns(c);
    int ow, oh;
    uint16_t *ovp = disp_overlay_px(c->ov, &ow, &oh);
    if (ovp) memset(ovp, 0, (size_t)ow * oh * 2);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(c->fd, VIDIOC_STREAMON, &type) != 0) { strcpy(c->error, "The camera did not start."); return false; }
    // esp_video's ISP pipeline turns its own tag up to debug whenever the build allows it, and the
    // sensor driver logs every exposure change: several lines a frame would bury the serial console
    esp_log_level_set("ISP", ESP_LOG_WARN);
    esp_log_level_set("ov02c10", ESP_LOG_WARN);
    c->done = xSemaphoreCreateBinary();
    if (xTaskCreatePinnedToCore(cam_task, "camera", 4096, c, CAM_PRIO, &c->task, SYS_CORE) != pdPASS) {
        strcpy(c->error, "The camera task could not start.");
        c->task = NULL;
        return false;
    }
    c->frames = c->frames_seen = 0;
    ESP_LOGI(TAG, "streaming %dx%d", c->fw, c->fh);
    return true;
}

// Stops the stream and gives back what cam_start took; the overlay stays unless free_overlay.
static void cam_stop(cam_t *c, bool free_overlay)
{
    if (c->task) {
        c->stop = true;
        // a picture being written finishes first; the dequeue times out, so this does not wait long
        if (xSemaphoreTake(c->done, pdMS_TO_TICKS(c->saving ? 15000 : 3000)) != pdTRUE) ESP_LOGW(TAG, "capture task did not stop");
        c->task = NULL;
    }
    if (c->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(c->fd, VIDIOC_STREAMOFF, &type);
        close(c->fd);
        c->fd = -1;
    }
    if (c->done) { vSemaphoreDelete(c->done); c->done = NULL; }
    free(c->shot);
    c->shot = NULL;
    if (free_overlay && c->ov >= 0) { disp_overlay_free(c->ov); c->ov = -1; }
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

static void show_state(cam_t *c, bool ok)
{
    ui_enable(c->shot_btn, ok);
    ui_enable(c->mirror, ok);
    ui_enable(c->res, s_nmodes > 1);
    if (!ok) { ESP_LOGW(TAG, "%s", c->error); ui_set_text(c->status, ""); }
    ui_invalidate(c->view);
}

// (Re)start the camera with the window up; it takes a moment to find and set up the sensor.
static void restart(cam_t *c)
{
    cam_stop(c, false);
    ui_invalidate(c->view);
    wm_pump(0);
    wm_busy(true);
    bool ok = cam_start(c);
    wm_busy(false);
    if (!ok) cam_stop(c, true);
    if (s_nmodes) { ui_dropdown_set_count(c->res, s_nmodes); ui_dropdown_select(c->res, s_mode); }
    show_state(c, ok);
}

static void take_picture(widget_t *b, int code)
{
    cam_t *c = b->win->user;
    if (!c->task || c->saving) return;
    c->saving = true;
    s_saved_msg[0] = 0;
    ui_set_text(c->status, "Saving...");
    c->want_shot = true;
}

static void mirror_changed(widget_t *b, int code)
{
    cam_t *c = b->win->user;
    c->mirror_on = ui_checked(b);
}

static void modes_get(widget_t *dd, int i, lv_item_t *it) { it->text = i < s_nmodes ? s_mode_label[i] : ""; }

static void res_changed(widget_t *dd, int code)
{
    cam_t *c = dd->win->user;
    int m = ui_dropdown_sel(dd);
    if (code != UI_SELECT || m < 0 || m == s_mode || c->saving) return;
    s_mode = m;
    s_saved_msg[0] = 0;
    restart(c);
}

static void tick(void *arg)
{
    cam_t *c = arg;
    if (s_cam != c || c->saving || !c->task) return;
    uint32_t n = c->frames, fps = n - c->frames_seen;
    c->frames_seen = n;
    ESP_LOGD(TAG, "%u fps, %u us a frame in the capture task", (unsigned)fps, (unsigned)c->work_us);
    if (s_saved_msg[0]) return;     // keep showing where the last picture went
    char t[64];
    snprintf(t, sizeof(t), "%u frames per second", (unsigned)fps);
    ui_set_text(c->status, t);
}

static void cam_destroy(window_t *w)
{
    cam_t *c = w->user;
    if (c->timer) timer_del(c->timer);
    s_cam = NULL;
    cam_stop(c, true);
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
    int y = VIEW_H + 18;
    c->view = ui_canvas(w, 8, 8, VIEW_W, VIEW_H, &view_ops, false);
    c->shot_btn = ui_button(w, 8, y, 110, 26, "Take &picture", take_picture);
    c->res = ui_dropdown(w, 128, y + 2, 112, modes_get);
    c->res->on_action = res_changed;
    c->mirror = ui_checkbox(w, 252, y + 5, 70, "&Mirror", false);
    c->mirror->on_action = mirror_changed;
    c->status = ui_label(w, 330, y + 6, VIEW_W - 322, 14, "");
    w->def_btn = c->shot_btn;
    w->focus = c->shot_btn;
    wm_show(w);
    restart(c);
    c->timer = timer_add(1000, true, tick, c);
}
