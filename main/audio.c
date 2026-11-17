// Sound output: a PCM5102 DAC module on the board's expansion header (JP1: 3.3 V, GND, GPIO1-4),
// wired as in the esp32_usb_cd_player project. The DAC needs no control bus, only I2S.
//
// A mixer task runs at 44.1 kHz stereo. It mixes up to eight one-shot voices (system sounds, game
// effects; any sample rate, resampled by stepping) and one stream (the sound player), applies the
// master volume and writes to the I2S driver, whose blocking write paces the task.
//
// On a board with nothing to drive (BOARD_HAS_AUDIO = 0) the calls still exist and answer "no sound
// here": the callers - the system sounds, the sound player, loaded programs - are the same code.
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "board.h"
#include "audio.h"
#include "os.h"

// Saved settings, not hardware: every board has a volume and a switch for the system sounds, even
// one with nothing to drive, so the Settings window and the settings file stay the same shape.
int audio_volume = 30;
int audio_system_sounds = 1;

#if BOARD_HAS_AUDIO

#define RATE        44100
#define BLOCK       256
#define VOICES      8
#define RING_FRAMES 16384          // stream buffer, in source frames (power of two)

static const char *TAG = "audio";

typedef struct {
    const void *pcm;
    uint32_t frames, pos;       // pos = whole frames played
    uint32_t frac, step;        // 16.16 position fraction and step per output frame
    uint8_t fmt;
    int16_t vl, vr;             // 0..256
    int id;                     // 0 = free
    void *owner;
} voice_t;

static i2s_chan_handle_t s_tx;
static SemaphoreHandle_t s_lock;
static voice_t s_voice[VOICES];
static int s_next_id = 1;
static bool s_ok;

static struct {
    int16_t *ring;              // stereo frames
    volatile uint32_t rd, wr;   // frame counters (free running)
    uint32_t frac, step;
    int rate, channels;
    volatile bool open, paused;
    void *owner;
} s_st;

static int16_t *s_chime[CHIME_COUNT];
static int s_chime_len[CHIME_COUNT];
#define CHIME_RATE 22050

bool audio_ok(void) { return s_ok; }

// ---------------------------------------------------------------- mixer

static inline int sample_at(const voice_t *v, uint32_t i, int ch)
{
    switch (v->fmt) {
    case AUDIO_U8_MONO: return ((int)((const uint8_t *)v->pcm)[i] - 128) << 8;
    case AUDIO_S16_MONO: return ((const int16_t *)v->pcm)[i];
    default: return ((const int16_t *)v->pcm)[i * 2 + ch];
    }
}

static void mix_block(int32_t *acc)
{
    memset(acc, 0, BLOCK * 2 * sizeof(int32_t));
    for (int k = 0; k < VOICES; k++) {
        voice_t *v = &s_voice[k];
        if (!v->id) continue;
        for (int i = 0; i < BLOCK; i++) {
            if (v->pos >= v->frames) { v->id = 0; break; }
            acc[i * 2] += sample_at(v, v->pos, 0) * v->vl >> 8;
            acc[i * 2 + 1] += sample_at(v, v->pos, 1) * v->vr >> 8;
            v->frac += v->step;
            v->pos += v->frac >> 16;
            v->frac &= 0xFFFF;
        }
    }
    if (s_st.open && !s_st.paused) {
        uint32_t rd = s_st.rd, wr = s_st.wr;
        for (int i = 0; i < BLOCK && rd != wr; i++) {
            const int16_t *f = &s_st.ring[(rd & (RING_FRAMES - 1)) * 2];
            acc[i * 2] += f[0];
            acc[i * 2 + 1] += f[1];
            s_st.frac += s_st.step;
            uint32_t adv = s_st.frac >> 16;
            s_st.frac &= 0xFFFF;
            if (adv > wr - rd) adv = wr - rd;
            rd += adv;
        }
        s_st.rd = rd;
    }
}

static void make_chimes(void);
static volatile int s_chime_wanted = -1;    // asked for before the sounds were ready
static volatile bool s_chimes_ready;

static void mixer_task(void *arg)
{
    static int32_t acc[BLOCK * 2];
    static int16_t out[BLOCK * 2];
    make_chimes();
    s_chimes_ready = true;
    if (s_chime_wanted >= 0) audio_chime(s_chime_wanted);
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        mix_block(acc);
        xSemaphoreGive(s_lock);
        int vol = audio_volume < 0 ? 0 : audio_volume > 100 ? 100 : audio_volume;
        int gain = vol * vol * 256 / 10000;     // quadratic: even steps to the ear
        for (int i = 0; i < BLOCK * 2; i++) {
            int32_t s = acc[i] * gain >> 8;
            out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        }
        size_t done;
        i2s_channel_write(s_tx, out, sizeof(out), &done, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------- voices

int audio_play(const void *pcm, int frames, int rate, int fmt, int vol, int pan, void *owner)
{
    if (!s_ok || !pcm || frames <= 0 || rate < 1000 || rate > 192000) return 0;
    int id = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) {
        voice_t *v = &s_voice[k];
        if (v->id) continue;
        *v = (voice_t){.pcm = pcm, .frames = frames, .step = (uint32_t)(((uint64_t)rate << 16) / RATE), .fmt = fmt, .owner = owner};
        id = v->id = s_next_id++;
        if (s_next_id > 0x3FFFFFFF) s_next_id = 1;
        break;
    }
    xSemaphoreGive(s_lock);
    if (id) audio_voice_set(id, vol, pan);
    return id;
}

void audio_voice_set(int id, int vol, int pan)
{
    if (!s_ok || id <= 0) return;
    vol = vol < 0 ? 0 : vol > 255 ? 255 : vol;
    pan = pan < 0 ? 0 : pan > 255 ? 255 : pan;
    // equal level in the centre, one side fading out towards the other
    int l = pan <= 128 ? 256 : (255 - pan) * 2, r = pan >= 128 ? 256 : pan * 2;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) if (s_voice[k].id == id) { s_voice[k].vl = vol * l >> 8; s_voice[k].vr = vol * r >> 8; }
    xSemaphoreGive(s_lock);
}

bool audio_playing(int id)
{
    if (!s_ok || id <= 0) return false;
    for (int k = 0; k < VOICES; k++) if (s_voice[k].id == id) return true;
    return false;
}

void audio_stop(int id)
{
    if (!s_ok || id <= 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) if (s_voice[k].id == id) s_voice[k].id = 0;
    xSemaphoreGive(s_lock);
}

void audio_stop_owner(void *owner)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) if (s_voice[k].id && s_voice[k].owner == owner) s_voice[k].id = 0;
    if (s_st.open && s_st.owner == owner) s_st.open = false;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- stream

bool audio_stream_open(int rate, int channels, void *owner)
{
    if (!s_ok || rate < 4000 || rate > 96000 || channels < 1 || channels > 2) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = !s_st.open;
    if (ok) {
        s_st.rd = s_st.wr = 0;
        s_st.frac = 0;
        s_st.step = (uint32_t)(((uint64_t)rate << 16) / RATE);
        s_st.rate = rate; s_st.channels = channels;
        s_st.paused = false;
        s_st.owner = owner;
        s_st.open = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int audio_stream_write(const int16_t *pcm, int frames, int timeout_ms)
{
    if (!s_st.open) return 0;
    int done = 0;
    uint32_t t0 = os_uptime_ms();
    while (done < frames && s_st.open) {
        uint32_t space = RING_FRAMES - (s_st.wr - s_st.rd);
        if (!space) {
            if ((int)(os_uptime_ms() - t0) >= timeout_ms) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        uint32_t wr = s_st.wr;
        int n = frames - done < (int)space ? frames - done : (int)space;
        for (int i = 0; i < n; i++, wr++) {
            int16_t *f = &s_st.ring[(wr & (RING_FRAMES - 1)) * 2];
            if (s_st.channels == 2) { f[0] = pcm[(done + i) * 2]; f[1] = pcm[(done + i) * 2 + 1]; }
            else f[0] = f[1] = pcm[done + i];
        }
        s_st.wr = wr;
        done += n;
    }
    return done;
}

int audio_stream_pending_ms(void)
{
    if (!s_st.open || !s_st.rate) return 0;
    return (int)((uint64_t)(s_st.wr - s_st.rd) * 1000 / s_st.rate);
}

void audio_stream_pause(bool on) { s_st.paused = on; }

void audio_stream_close(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.open = false;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- system sounds

// A note: a sine with two soft overtones and an exponential decay, added into buf.
static void note(int16_t *buf, int len, float start_s, float freq, float dur_s, float amp)
{
    int a = (int)(start_s * CHIME_RATE), n = (int)(dur_s * CHIME_RATE);
    for (int i = 0; i < n && a + i < len; i++) {
        float t = (float)i / CHIME_RATE;
        float env = expf(-t * 5.0f / dur_s) * (i < 60 ? i / 60.0f : 1.0f);
        float ph = 2.0f * (float)M_PI * freq * t;
        float s = sinf(ph) + 0.3f * sinf(2 * ph) + 0.12f * sinf(3 * ph);
        int v = buf[a + i] + (int)(s * env * amp);
        buf[a + i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
}

static int16_t *chime_buf(int which, float seconds)
{
    int len = (int)(seconds * CHIME_RATE);
    int16_t *b = heap_caps_calloc(len, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_chime[which] = b;
    s_chime_len[which] = b ? len : 0;
    return b;
}

static void make_chimes(void)
{
    int16_t *b;
    if ((b = chime_buf(CHIME_STARTUP, 1.6f))) {
        int n = s_chime_len[CHIME_STARTUP];
        note(b, n, 0.00f, 392.00f, 0.9f, 5200);   // G4
        note(b, n, 0.14f, 493.88f, 0.9f, 5200);   // B4
        note(b, n, 0.28f, 587.33f, 0.9f, 5200);   // D5
        note(b, n, 0.42f, 783.99f, 1.1f, 6000);   // G5
    }
    if ((b = chime_buf(CHIME_INFO, 0.5f))) note(b, s_chime_len[CHIME_INFO], 0, 880.0f, 0.45f, 7000);
    if ((b = chime_buf(CHIME_ERROR, 0.6f))) {
        note(b, s_chime_len[CHIME_ERROR], 0.00f, 311.13f, 0.30f, 8000);
        note(b, s_chime_len[CHIME_ERROR], 0.16f, 233.08f, 0.42f, 8000);
    }
    if ((b = chime_buf(CHIME_SHUTTER, 0.12f))) {
        uint32_t r = 12345;
        for (int i = 0; i < s_chime_len[CHIME_SHUTTER]; i++) {
            r = r * 1664525u + 1013904223u;
            float env = expf(-(float)i / CHIME_RATE * 60.0f);
            b[i] = (int16_t)(((int)(r >> 16) - 32768) * env * 0.25f);
        }
    }
}

void audio_chime(int which)
{
    if (!s_ok || !audio_system_sounds || which < 0 || which >= CHIME_COUNT) return;
    if (!s_chimes_ready) { s_chime_wanted = which; return; }
    if (!s_chime[which]) return;
    audio_play(s_chime[which], s_chime_len[which], CHIME_RATE, AUDIO_S16_MONO, 255, 128, NULL);
}

// ---------------------------------------------------------------- init

void audio_start(void)
{
    // the on-board speaker amplifier has no input in this configuration: keep it off
    const gpio_config_t pa = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = BIT64(BOARD_PIN_PA_EN)};
    gpio_config(&pa);
    gpio_set_level(BOARD_PIN_PA_EN, 0);

    s_lock = xSemaphoreCreateMutex();
    s_st.ring = heap_caps_malloc(RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_st.ring) return;

    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ch.dma_desc_num = 6;
    ch.dma_frame_num = BLOCK;
    ch.auto_clear = true;
    if (i2s_new_channel(&ch, &s_tx, NULL) != ESP_OK) { ESP_LOGE(TAG, "no I2S channel"); return; }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) { ESP_LOGE(TAG, "I2S setup failed"); return; }
    s_ok = true;
    xTaskCreatePinnedToCore(mixer_task, "mixer", 4096, NULL, 12, NULL, SYS_CORE);
    ESP_LOGI(TAG, "PCM5102 on I2S: LRCK %d, DIN %d, BCK %d, SCK %d", BOARD_I2S_WS, BOARD_I2S_DOUT, BOARD_I2S_BCLK, BOARD_I2S_MCLK);
}

#else  // BOARD_HAS_AUDIO == 0: no codec, no amplifier, nothing to write samples to

// The same answers as above, from a mixer that does not exist: a voice request fails, a stream
// accepts nothing, the system sounds stay silent. Callers ask audio_ok() first and say "there is no
// sound output here" when it answers false.
void audio_start(void) { }
bool audio_ok(void) { return false; }

int audio_play(const void *pcm, int frames, int rate, int fmt, int vol, int pan, void *owner) { return 0; }
void audio_voice_set(int id, int vol, int pan) { }
bool audio_playing(int id) { return false; }
void audio_stop(int id) { }
void audio_stop_owner(void *owner) { }

bool audio_stream_open(int rate, int channels, void *owner) { return false; }
int audio_stream_write(const int16_t *pcm, int frames, int timeout_ms) { return 0; }
int audio_stream_pending_ms(void) { return 0; }
void audio_stream_pause(bool on) { }
void audio_stream_close(void) { }

void audio_chime(int which) { }

#endif
