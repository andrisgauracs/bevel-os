// Sound Player: plays WAV files (PCM, 8 or 16 bit, mono or stereo, any usual sample rate) through
// the mixer's stream. One player at a time; opening another file switches to it.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "fsutil.h"
#include "audio.h"

typedef struct {
    window_t *win;
    widget_t *name, *info, *bar, *play_btn;
    int timer;
    char path[300];
    // playback state, shared with the reader task
    volatile bool run, paused, task_alive, finished;
    volatile uint32_t played_frames;
    uint32_t total_frames;
    int rate, channels, bits;
    long data_off;
    const char *error;
} player_t;

static player_t *s_pl;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Find the format and the samples. Returns NULL, or what is wrong with the file.
static const char *wav_open(player_t *p, FILE *f)
{
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return "This is not a WAV sound file.";
    bool have_fmt = false;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) return "The sound file has no samples in it.";
        uint32_t len = rd32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) return "The sound file is damaged.";
            int tag = fm[0] | fm[1] << 8;
            p->channels = fm[2] | fm[3] << 8;
            p->rate = rd32(fm + 4);
            p->bits = fm[14] | fm[15] << 8;
            if (tag != 1 && tag != 0xFFFE) return "Only uncompressed (PCM) WAV files can be played.";
            if (p->channels < 1 || p->channels > 2 || (p->bits != 8 && p->bits != 16) || p->rate < 4000 || p->rate > 96000)
                return "This WAV format is not supported (8 or 16 bit, mono or stereo, up to 96 kHz).";
            fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
            have_fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt) return "The sound file is damaged.";
            p->data_off = ftell(f);
            p->total_frames = len / (p->channels * p->bits / 8);
            return NULL;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
}

static void reader_task(void *arg)
{
    player_t *p = arg;
    enum { CHUNK = 2048 };
    FILE *f = fopen(p->path, "rb");
    uint8_t *raw = heap_caps_malloc(CHUNK * 4, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int fsz = p->channels * p->bits / 8;
    if (f && raw && pcm && audio_stream_open(p->rate, p->channels, p)) {
        fseek(f, p->data_off, SEEK_SET);
        uint32_t left = p->total_frames;
        while (p->run && left) {
            if (p->paused) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
            int n = left < CHUNK ? (int)left : CHUNK;
            n = fread(raw, fsz, n, f);
            if (n <= 0) break;
            const int16_t *src = (const int16_t *)raw;
            if (p->bits == 8) {
                for (int i = 0; i < n * p->channels; i++) pcm[i] = (int16_t)((raw[i] - 128) << 8);
                src = pcm;
            }
            int done = 0;
            while (done < n && p->run) done += audio_stream_write(src + done * p->channels, n - done, 100);
            left -= n;
            p->played_frames += n;
        }
        // let the buffered tail play out
        while (p->run && audio_stream_pending_ms() > 0) vTaskDelay(pdMS_TO_TICKS(20));
        audio_stream_close();
        if (p->run) p->finished = true;
    }
    if (f) fclose(f);
    free(raw); free(pcm);
    p->task_alive = false;
    vTaskDelete(NULL);
}

static void pl_stop(player_t *p)
{
    p->run = false;
    audio_stream_pause(false);
    while (p->task_alive) vTaskDelay(pdMS_TO_TICKS(5));
    p->paused = false;
    p->played_frames = 0;
}

static void pl_start(player_t *p)
{
    pl_stop(p);
    if (!audio_ok()) { msgbox(p->win, "Sound Player", "The sound output could not be started.", MB_ERROR, MB_OK); return; }
    p->run = true; p->finished = false; p->task_alive = true;
    if (xTaskCreatePinnedToCore(reader_task, "player", 4096, p, 4, NULL, SYS_CORE) != pdPASS) { p->task_alive = false; p->run = false; }
}

static void fmt_time(char *b, int n, uint32_t frames, int rate)
{
    uint32_t s = rate ? frames / rate : 0;
    snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void bar_paint(widget_t *w, gfx_t *g)
{
    player_t *p = w->win->user;
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    gfx_fill(g, 2, 2, w->r.w - 4, w->r.h - 4, C_FIELD);
    uint32_t played = p->played_frames;
    uint32_t lag = (uint32_t)((uint64_t)audio_stream_pending_ms() * p->rate / 1000);   // read, but not heard yet
    played = played > lag ? played - lag : 0;
    if (p->total_frames) {
        int fill = (int)((uint64_t)(w->r.w - 4) * played / p->total_frames);
        if (fill > w->r.w - 4) fill = w->r.w - 4;
        gfx_fill(g, 2, 2, fill, w->r.h - 4, C_SEL);
    }
}

static void bar_mouse(widget_t *w, int type, int x, int y, int btn, int clicks) { }
static const canvas_ops_t bar_ops = {bar_paint, NULL, bar_mouse};

static void pl_refresh(void *arg)
{
    player_t *p = s_pl;
    if (!p) return;
    if (p->finished) { p->finished = false; p->run = false; p->played_frames = 0; }
    char a[16], b[16], t[96];
    uint32_t played = p->played_frames;
    uint32_t lag = (uint32_t)((uint64_t)audio_stream_pending_ms() * p->rate / 1000);
    fmt_time(a, sizeof(a), played > lag ? played - lag : 0, p->rate);
    fmt_time(b, sizeof(b), p->total_frames, p->rate);
    snprintf(t, sizeof(t), "%s / %s      %d Hz, %d bit, %s", a, b, p->rate, p->bits, p->channels == 2 ? "stereo" : "mono");
    ui_set_text(p->info, t);
    ui_set_text(p->play_btn, p->run && !p->paused ? "&Pause" : "&Play");
    ui_invalidate(p->bar);
}

static void do_play(widget_t *b, int code)
{
    player_t *p = b->win->user;
    if (!p->run) pl_start(p);
    else { p->paused = !p->paused; audio_stream_pause(p->paused); }
    pl_refresh(NULL);
}

static void do_stop(widget_t *b, int code) { pl_stop(b->win->user); pl_refresh(NULL); }

static void do_open(widget_t *b, int code)
{
    player_t *p = b->win->user;
    char path[300];
    strlcpy(path, p->path[0] ? p->path : DISK_ROOT "/Music", sizeof(path));
    char *slash = strrchr(path, '/');
    if (p->path[0] && slash) *slash = 0;
    if (file_dialog(p->win, false, "Open sound", path, sizeof(path))) app_player(path);
}

static bool pl_load(player_t *p, const char *path)
{
    pl_stop(p);
    FILE *f = fopen(path, "rb");
    const char *err = f ? wav_open(p, f) : "The file could not be opened.";
    if (f) fclose(f);
    if (err) { msgbox(p->win, "Sound Player", err, MB_ERROR, MB_OK); return false; }
    strlcpy(p->path, path, sizeof(p->path));
    ui_set_text(p->name, fs_basename(path));
    return true;
}

static void pl_destroy(window_t *w)
{
    player_t *p = w->user;
    pl_stop(p);
    timer_del(p->timer);
    s_pl = NULL;
    free(p);
}

void app_player(const char *arg)
{
    player_t *p = s_pl;
    if (!p) {
        p = calloc(1, sizeof(player_t));
        if (!p) return;
        window_t *w = wm_create("Sound Player", 340, 112, 0);
        w->icon = &icon16_sound;
        w->user = p;
        p->win = w;
        p->rate = 44100; p->bits = 16; p->channels = 2;
        ui_icon(w, 10, 8, &icon32_sound);
        p->name = ui_label(w, 52, 10, 280, 14, "No sound loaded");
        p->info = ui_label(w, 52, 28, 280, 14, "");
        p->bar = ui_canvas(w, 10, 52, 320, 16, &bar_ops, false);
        p->play_btn = ui_button(w, 10, 78, 78, 24, "&Play", do_play);
        ui_button(w, 94, 78, 78, 24, "&Stop", do_stop);
        ui_button(w, 252, 78, 78, 24, "&Open...", do_open);
        w->on_destroy = pl_destroy;
        w->focus = p->play_btn;
        s_pl = p;
        p->timer = timer_add(250, true, pl_refresh, NULL);
        wm_show(w);
    } else {
        wm_activate(p->win);
    }
    if (arg && arg[0] && pl_load(p, arg)) pl_start(p);
    pl_refresh(NULL);
}
