// Doom on Bevel OS: the platform layer for doomgeneric (https://github.com/ozkl/doomgeneric, GPL-2).
// The engine renders 320x200 in 256 colours, shown at 640x400. Where the board has a true-colour
// layer (the P4's DSI panel) every game pixel becomes a 2x2 block of RGB565, so the game's own palette
// is used as it is. Without one (the S3's VGA output, 64 colours) the four screen pixels of each game
// pixel carry a 2x2 ordered dither instead, which gives five shades between each pair of real colour
// levels at no loss of resolution. Sound effects go to the OS mixer where the board has sound; there
// is no music (that would need a MIDI synthesiser).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../bevel.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

struct color { uint32_t b:8; uint32_t g:8; uint32_t r:8; uint32_t a:8; };
extern struct color colors[256];
extern int palette_changed;   // boolean in the engine

static const bevel_api_t *os;
static uint16_t *s_px;              // the true-colour layer, or NULL
static uint8_t *s_canvas;           // otherwise the RGB222 canvas
static int s_cw, s_ch;
static uint16_t s_lut[256];         // RGB565 per palette entry, or for the canvas two dithered pixels:
static uint16_t s_lut_bot[256];     // s_lut for the even screen rows, s_lut_bot for the odd ones

#define KQ 64
static struct { uint8_t pressed, key; } s_keys[KQ];
static int s_kr, s_kw;

void bevel_exit(void);

// One channel of a game colour at one of the four positions of a 2x2 block, as an RGB222 pixel.
static uint8_t quant(int r, int g, int b, int pos)
{
    static const int off[4] = {-32, 11, 32, -11};   // 2x2 Bayer thresholds, in units of 1/255
    int c[3] = {r, g, b};
    uint8_t p = 0;
    for (int k = 0; k < 3; k++) {
        int v = c[k] + off[pos];
        int q = (v + 42) / 85;
        if (v < 0) q = 0;
        if (q > 3) q = 3;
        p |= q << (k * 2);
    }
    return p;
}

void DG_Init(void)
{
    // The canvas has to exist before the true-colour layer for it can be allocated, so the window
    // comes first.
    int w = DOOMGENERIC_RESX * 2, h = DOOMGENERIC_RESY * 2;
    if (os->win_open("Doom", w, h) != 0) bevel_exit();
    s_px = os->canvas16(&s_cw, &s_ch);
    if (s_px && (s_cw != w || s_ch != h)) s_px = NULL;
    if (!s_px) {
        s_canvas = os->canvas(&s_cw, &s_ch);
        if (!s_canvas || s_cw < w || s_ch < h) bevel_exit();
    }
}

static void draw_dithered(void)
{
    const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;
    uint16_t top[DOOMGENERIC_RESX], bot[DOOMGENERIC_RESX];
    for (int y = 0; y < DOOMGENERIC_RESY; y++, src += DOOMGENERIC_RESX) {
        for (int x = 0; x < DOOMGENERIC_RESX; x++) { uint8_t i = src[x]; top[x] = s_lut[i]; bot[x] = s_lut_bot[i]; }
        memcpy(s_canvas + (y * 2) * s_cw, top, DOOMGENERIC_RESX * 2);
        memcpy(s_canvas + (y * 2 + 1) * s_cw, bot, DOOMGENERIC_RESX * 2);
    }
    os->flush();
}

void DG_DrawFrame(void)
{
    if (palette_changed) {
        for (int i = 0; i < 256; i++) {
            int r = colors[i].r, g = colors[i].g, b = colors[i].b;
            if (s_px) s_lut[i] = BV_RGB565(r, g, b);
            else { s_lut[i] = quant(r, g, b, 0) | quant(r, g, b, 1) << 8; s_lut_bot[i] = quant(r, g, b, 2) | quant(r, g, b, 3) << 8; }
        }
        palette_changed = 0;
    }
    if (!s_px) { draw_dithered(); return; }
    // every game pixel becomes a 2x2 block of RGB565, so the 320x200 frame exactly fills the layer
    const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;
    for (int y = 0; y < DOOMGENERIC_RESY; y++, src += DOOMGENERIC_RESX) {
        uint16_t *d = s_px + (y * 2) * s_cw;
        for (int x = 0; x < DOOMGENERIC_RESX; x++) { uint16_t c = s_lut[src[x]]; d[x * 2] = c; d[x * 2 + 1] = c; }
        memcpy(d + s_cw, d, DOOMGENERIC_RESX * 2 * sizeof(uint16_t));
    }
    os->flush16();
}

// ---------------------------------------------------------------- sound effects
// The engine's sound module interface, on top of the OS mixer's one-shot voices. A sound lump is
// 8-bit unsigned mono at its own rate (11025 Hz mostly): the mixer plays it straight from the
// engine's lump cache.

#define SND_CHANNELS 16
static int s_voice[SND_CHANNELS];
static boolean s_sfx_prefix;

int use_libsamplerate = 0;          // configuration variables the engine binds when sound is built in
float libsamplerate_scale = 0.65f;

static boolean snd_init(boolean use_sfx_prefix) { s_sfx_prefix = use_sfx_prefix; return true; }
static void snd_shutdown(void) { for (int i = 0; i < SND_CHANNELS; i++) os->snd_stop(s_voice[i]); }

static int snd_lump(sfxinfo_t *sfx)
{
    char name[12];
    if (sfx->link) sfx = sfx->link;
    snprintf(name, sizeof(name), "%s%s", s_sfx_prefix ? "ds" : "", sfx->name);
    return W_GetNumForName(name);
}

static void snd_update(void) { }

// vol 0..127, sep 0 (left)..254 (right)
static void snd_params(int channel, int vol, int sep)
{
    if (channel >= 0 && channel < SND_CHANNELS) os->snd_set(s_voice[channel], vol * 2, sep);
}

static int snd_start(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= SND_CHANNELS) return -1;
    os->snd_stop(s_voice[channel]);
    int lump = sfx->lumpnum;
    int len = W_LumpLength(lump);
    const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC);     // stays in the cache: the mixer reads it while it plays
    if (len < 8 + 32 + 1 || d[0] != 3 || d[1] != 0) return -1;
    int rate = d[2] | d[3] << 8;
    int n = d[4] | d[5] << 8 | d[6] << 16 | d[7] << 24;
    if (n > len - 8) n = len - 8;
    if (n <= 32) return -1;
    // the first and last 16 samples are padding
    s_voice[channel] = os->snd_play(d + 8 + 16, n - 32, rate, BV_SND_U8_MONO, vol * 2, sep);
    return s_voice[channel] ? channel : -1;
}

static void snd_stop(int channel) { if (channel >= 0 && channel < SND_CHANNELS) { os->snd_stop(s_voice[channel]); s_voice[channel] = 0; } }
static boolean snd_playing(int channel) { return channel >= 0 && channel < SND_CHANNELS && os->snd_playing(s_voice[channel]); }

static snddevice_t s_snd_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};
sound_module_t DG_sound_module = {
    s_snd_devices, sizeof(s_snd_devices) / sizeof(s_snd_devices[0]),
    snd_init, snd_shutdown, snd_lump, snd_update, snd_params, snd_start, snd_stop, snd_playing, NULL,
};

// no music
static boolean mus_init(void) { return false; }
static void mus_void(void) { }
static void mus_volume(int v) { }
static void *mus_register(void *data, int len) { return NULL; }
static void mus_unregister(void *h) { }
static void mus_play(void *h, boolean looping) { }
static boolean mus_playing(void) { return false; }
music_module_t DG_music_module = {
    NULL, 0, mus_init, mus_void, mus_volume, mus_void, mus_void, mus_register, mus_unregister, mus_play, mus_void, mus_playing, NULL,
};

void DG_SleepMs(uint32_t ms) { os->sleep_ms(ms); }
uint32_t DG_GetTicksMs(void) { return os->ticks_ms(); }
void DG_SetWindowTitle(const char *title) { (void)title; }

static uint8_t map_key(const bv_event_t *e)
{
    switch (e->key) {
    case 0x4F: return KEY_RIGHTARROW;
    case 0x50: return KEY_LEFTARROW;
    case 0x52: return KEY_UPARROW;
    case 0x51: return KEY_DOWNARROW;
    case 0xE0: case 0xE4: return KEY_FIRE;      // Ctrl
    case 0x2C: return KEY_USE;                  // Space
    case 0xE1: case 0xE5: return KEY_RSHIFT;    // Shift = run
    case 0x28: case 0x58: return KEY_ENTER;
    case 0x29: return KEY_ESCAPE;
    case 0x2B: return KEY_TAB;                  // map
    case 0x2A: return KEY_BACKSPACE;
    case 0x36: return KEY_STRAFE_L;             // ,
    case 0x37: return KEY_STRAFE_R;             // .
    case 0x2E: return KEY_EQUALS;
    case 0x2D: return KEY_MINUS;
    case 0x3B: return KEY_F2; case 0x3C: return KEY_F3; case 0x3E: return KEY_F5; case 0x3F: return KEY_F6;
    case 0x40: return KEY_F7; case 0x41: return KEY_F8; case 0x42: return KEY_F9; case 0x44: return KEY_F11;
    }
    if (e->key >= 0x04 && e->key <= 0x1D) return 'a' + (e->key - 0x04);
    if (e->key >= 0x1E && e->key <= 0x26) return '1' + (e->key - 0x1E);
    if (e->key == 0x27) return '0';
    return 0;
}

static void pump(void)
{
    bv_event_t e;
    while (os->event(&e, 0)) {
        if (e.type == BV_CLOSE) bevel_exit();
        if (e.type != BV_KEY_DOWN && e.type != BV_KEY_UP) continue;
        if (e.repeat) continue;
        uint8_t k = map_key(&e);
        if (!k) continue;
        int next = (s_kw + 1) % KQ;
        if (next == s_kr) continue;
        s_keys[s_kw].pressed = e.type == BV_KEY_DOWN;
        s_keys[s_kw].key = k;
        s_kw = next;
    }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    pump();
    if (s_kr == s_kw) return 0;
    *pressed = s_keys[s_kr].pressed;
    *key = s_keys[s_kr].key;
    s_kr = (s_kr + 1) % KQ;
    return 1;
}

void bevel_exit(void) { exit(0); }

int app_main(const bevel_api_t *api, const char *arg)
{
    os = api;
    if (os->version < 2) { os->msgbox("Doom", "This build of Doom needs a newer Bevel OS.", BV_MB_OK); return 1; }
    // the game data: on the drive, or on the SD card
    static const char *const places[] = {"/disk/Apps/doom1.wad", "/disk/SD/doom1.wad", "/disk/SD/Apps/doom1.wad", "/disk/SD/DOOM1.WAD"};
    // the zone heap: 8 MB on the P4 (32 MB of PSRAM), 4 MB on the S3, which has 6 MB free in all
#if BV_ARCH_RISCV
    static char *argv[] = {"doom", "-iwad", NULL, "-mb", "8", NULL};
#else
    static char *argv[] = {"doom", "-iwad", NULL, "-mb", "4", NULL};
#endif
    for (int i = 0; i < 4 && !argv[2]; i++) {
        FILE *f = fopen(places[i], "rb");
        if (f) { fclose(f); argv[2] = (char *)places[i]; }
    }
    if (!argv[2]) {
        os->msgbox("Doom", "The game data file doom1.wad is missing. Put it in the Apps folder of the drive, or on the SD card.", BV_MB_OK);
        return 1;
    }
    doomgeneric_Create(5, argv);
    for (;;) doomgeneric_Tick();
}
