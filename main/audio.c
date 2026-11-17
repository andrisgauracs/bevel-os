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

// The mixer has to keep up with the DAC, so it runs above everything that draws and above the USB and
// touch tasks it shares a core with (usb_lib 10, hid and touch 8). That is right while it is mixing and
// wrong for the first stretch of its life: synthesising the four system sounds used to take 2.3 s of pure
// arithmetic, and at that priority it starved those input tasks for as long as it took - it showed up at
// boot as a pointer that answered, then stopped, and started again at the exact moment the startup sound
// did, the same whether or not the volume was up because the sounds are built either way. Synthesis is
// 40 ms now (the log line below says how long it took, so a slower build is visible), and it still runs
// below the input tasks: if that number ever grows again, this task should not pay for it at mixer
// priority.
#define MIXER_PRIO  12
#define BUILD_PRIO   4

static void mixer_task(void *arg)
{
    static int32_t acc[BLOCK * 2];
    static int16_t out[BLOCK * 2];
    uint32_t build_t0 = os_uptime_ms();
    vTaskPrioritySet(NULL, BUILD_PRIO);   // synthesise out of the input tasks' way...
    make_chimes();
    int peak = 0, tail = 0;   // a chime that decays to nothing still "builds" quickly and reports no error,
                              // so check the sound: the whole peak, and the peak of its last tenth, which is
                              // where a broken decay dies first (the version below logged 9229 and 0)
    if (s_chime[CHIME_STARTUP]) {
        int n = s_chime_len[CHIME_STARTUP], from = n - n / 10;
        for (int i = 0; i < n; i++) {
            int v = s_chime[CHIME_STARTUP][i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
            if (i >= from && v > tail) tail = v;
        }
    }
    ESP_LOGI(TAG, "chimes built in %u ms, startup sound peaks at %d, its last tenth at %d of 32767",
             (unsigned)(os_uptime_ms() - build_t0), peak, tail);
    vTaskPrioritySet(NULL, MIXER_PRIO);   // ...then take the priority the DAC expects
    // Ready goes last and the parked request is taken straight after it. audio_chime runs on the other core:
    // a request that arrives while this task is still busy would otherwise be parked and never picked up.
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
//
// The obvious way to write this - the sine of a steadily growing angle, and an exponential, once per
// sample - cost 2.3 s of one core here, which is long enough to be in the way of the boot (see
// MIXER_PRIO below). The same sound is a few cycles a sample if the angle walks a small table by
// accumulation and the envelope is one running multiply.
#define SINE_BITS 11                        // 2048 entries, 4 kB; at 880 Hz the phase advances 82 entries a sample
#define SINE_STEPS (1 << SINE_BITS)
static int16_t s_sine[SINE_STEPS];

static void make_sine_table(void)
{
    for (int i = 0; i < SINE_STEPS; i++)
        s_sine[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * (float)i / SINE_STEPS));
}

static void note(int16_t *buf, int len, float start_s, float freq, float dur_s, float amp)
{
    int a = (int)(start_s * CHIME_RATE), n = (int)(dur_s * CHIME_RATE);
    uint32_t phase = 0;
    uint32_t step = (uint32_t)(freq * (double)4294967296.0 / CHIME_RATE);   // phase per sample, one lap = 2^32
    int env = 32767;                                    // Q15, kept below 32768 so env * dec stays inside 31 bits
    // Per sample, not per note: the decay time constant is dur_s/5 seconds, so one sample is a factor of
    // exp(-5 / (dur_s * CHIME_RATE)). Dropping the sample rate here - which is what this line first said -
    // decays the note to silence in two samples, and the chime is inaudible while still being "built".
    // Q16 with the discarded remainder carried, because Q15 is too coarse for a factor this close to one:
    // truncated to 32759 it ran 5.45 e-folds instead of 5.00 (3.9 dB gone by the end of the note), and once
    // the step it removes falls below one count the multiply stops decaying at all and the rest of the note
    // drains to nothing - the 0.9 s notes went silent at 0.47 s, and the startup sound lost its last 0.63 s.
    int dec = (int)(65536.0f * expf(-5.0f / (dur_s * (float)CHIME_RATE)) + 0.5f);
    int carry = 0;
    int a2 = (int)amp >> 4;                             // pre-scaled so the sums stay inside 32 bits
    for (int i = 0; i < n && a + i < len; i++) {
        int p = (int)(phase >> (32 - SINE_BITS));       // 2^32 / SINE_STEPS per table step
        int s = s_sine[p] + 3 * (int)s_sine[(2 * p) & (SINE_STEPS - 1)] / 10
                            + 3 * (int)s_sine[(3 * p) & (SINE_STEPS - 1)] / 25;   // 0.3 and 0.12 of the fundamental
        int add = ((s * a2) >> 11) * env >> 15;
        if (i < 60) add = add * i / 60;                 // the same soft attack ramp
        int v = buf[a + i] + add;
        buf[a + i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        phase += step;
        int e = env * dec;                              // 32767 * 65535 at most: fits
        env = e >> 16;
        carry += e & 0xFFFF;                            // put back what the shift threw away, one count at a time
        if (carry >= 65536) { env++; carry -= 65536; }
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
    make_sine_table();
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
        int env = 32768, dec = (int)(32768.0f * expf(-60.0f / CHIME_RATE));
        for (int i = 0; i < s_chime_len[CHIME_SHUTTER]; i++) {
            r = r * 1664525u + 1013904223u;
            b[i] = (int16_t)((((int)(r >> 16) - 32768) * env >> 15) >> 2);
            env = env * dec >> 15;
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
    xTaskCreatePinnedToCore(mixer_task, "mixer", 4096, NULL, MIXER_PRIO, NULL, SYS_CORE);
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
