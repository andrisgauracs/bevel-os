// Bevel OS application interface. An app needs only this header.
//
//   #include "bevel.h"
//   int app_main(const bevel_api_t *os, const char *arg) { ... }
//
// Build with tools/build_apps.sh, which produces Apps/<name>.elf for the drive. The app runs in
// its own task. It draws into its window's canvas with the calls below and receives input
// through os->event(). All coordinates are canvas pixels; colours are RGB222 (see BV_RGB).
#pragma once
#include <stdint.h>

#define BEVEL_API_VERSION 2
#define BV_RGB(r, g, b) ((uint8_t)((((b) & 3) << 4) | (((g) & 3) << 2) | ((r) & 3)))  // each 0..3

enum { BV_NONE = 0, BV_KEY_DOWN, BV_KEY_UP, BV_MOUSE_DOWN, BV_MOUSE_UP, BV_MOUSE_MOVE, BV_CLOSE };
enum { BV_FONT_UI = 0, BV_FONT_BOLD = 1, BV_FONT_MONO = 2 };
enum { BV_MOD_CTRL = 1, BV_MOD_SHIFT = 2, BV_MOD_ALT = 4 };
// key codes are USB HID usages
enum { BV_K_A = 0x04, BV_K_D = 0x07, BV_K_N = 0x11, BV_K_P = 0x13, BV_K_R = 0x15, BV_K_S = 0x16, BV_K_W = 0x1A,
       BV_K_ENTER = 0x28, BV_K_ESC = 0x29, BV_K_SPACE = 0x2C,
       BV_K_RIGHT = 0x4F, BV_K_LEFT = 0x50, BV_K_DOWN = 0x51, BV_K_UP = 0x52 };
enum { BV_SND_U8_MONO = 0, BV_SND_S16_MONO = 1, BV_SND_S16_STEREO = 2 };
#define BV_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))   // each 0..255
enum { BV_MB_OK = 1, BV_MB_YESNO = 3, BV_ID_OK = 1, BV_ID_YES = 3, BV_ID_NO = 4 };

typedef struct {
    uint8_t type;       // BV_*
    uint8_t key;        // HID usage (key events)
    char ch;            // printable character, or 0
    uint8_t mods;       // BV_MOD_*
    uint8_t button;     // 1 left, 2 right, 4 middle
    uint8_t repeat;     // key auto-repeat
    int16_t x, y;       // mouse position in the canvas
} bv_event_t;

typedef struct bevel_api {
    uint32_t version;
    // window: one per app, with a canvas of w x h pixels
    int  (*win_open)(const char *title, int w, int h);          // 0 on success
    void (*win_title)(const char *title);
    // input: waits up to timeout_ms (0 = poll, -1 = forever). Returns 1 and fills ev, or 0.
    int  (*event)(bv_event_t *ev, int timeout_ms);
    // drawing (into the canvas; call flush to show it)
    void (*clear)(int color);
    void (*fill)(int x, int y, int w, int h, int color);
    void (*frame)(int x, int y, int w, int h, int color);
    void (*pixel)(int x, int y, int color);
    void (*line)(int x0, int y0, int x1, int y1, int color);
    int  (*text)(int x, int y, const char *s, int font, int color);  // returns the width drawn
    int  (*text_width)(const char *s, int font);
    void (*blit)(int x, int y, int w, int h, const uint8_t *pixels); // 0xFF = transparent
    void (*bevel)(int x, int y, int w, int h, int raised);
    void (*flush)(void);
    void (*flush_rect)(int x, int y, int w, int h);
    // files (paths are relative to the drive root, e.g. "/Documents/score.txt")
    int  (*file_read)(const char *path, void *buf, int max);     // bytes read, or -1
    int  (*file_write)(const char *path, const void *buf, int len); // 0 on success
    // time and misc
    uint32_t (*ticks_ms)(void);
    void (*sleep_ms)(int ms);
    uint32_t (*random)(void);
    void *(*alloc)(int bytes);                                   // freed automatically when the app ends
    void (*free)(void *p);
    int  (*msgbox)(const char *title, const char *text, int buttons);
    void (*log)(const char *text);                               // to the serial console
    // direct access to the canvas: w x h bytes, one RGB222 pixel each, row stride = w
    uint8_t *(*canvas)(int *w, int *h);

    // ---- API version 2 ----
    // sound: play a block of samples once. format is BV_SND_*, volume 0..255, pan 0 (left)..255
    // (right). The samples must stay where they are while they play. Returns a voice (> 0), or 0.
    int  (*snd_play)(const void *pcm, int frames, int rate, int format, int volume, int pan);
    void (*snd_set)(int voice, int volume, int pan);
    int  (*snd_playing)(int voice);
    void (*snd_stop)(int voice);
    // true colour: a layer of w x h RGB565 pixels that replaces the canvas on screen (the drawing
    // calls above no longer show). Write pixels, then call flush16.
    uint16_t *(*canvas16)(int *w, int *h);
    void (*flush16)(void);
} bevel_api_t;

int app_main(const bevel_api_t *os, const char *arg);
