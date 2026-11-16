// Bevel OS desktop simulator: the OS compiled for Linux, its framebuffer shown in an SDL window.
//
//   bevel-sim [--scale N] [--disk DIR] [--serial-port P] [--headless] [--raw-keys] [--quiet]
//
// The OS runs exactly as on the board (same sources, FreeRTOS tasks as threads); only the
// drivers differ: the display is the headless backend presented here, keyboard and mouse come
// from SDL events, the drive is a folder, the network is the host's, apps are x86-64 builds.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <SDL.h>
#include "host_port.h"
#include "host_sim.h"
#include "vga.h"
#include "input.h"

char **host_argv;
char host_disk[1024];
int host_serial_port = 47812;
bool host_echo_serial = true;

void app_main(void);

static void usage(void)
{
    fprintf(stderr,
            "usage: bevel-sim [options]\n"
            "  --scale N         window size: 640x480 times N (default 2)\n"
            "  --disk DIR        folder used as the drive /disk (default: %s)\n"
            "  --serial-port P   TCP port of the serial command channel (default 47812)\n"
            "  --headless        no window (use VNC or the serial channel)\n"
            "  --raw-keys        send physical keys (US layout, like a USB keyboard on the board)\n"
            "                    instead of the characters typed with the host's layout\n"
            "  --quiet           do not copy the serial log to the terminal\n", BEVEL_DEFAULT_DISK);
    exit(2);
}

// RGB222 -> ARGB8888, with the levels of the resistor DAC (0, 0.2, 0.45, 0.65 V of 0.7 V)
static uint32_t s_lut[64];
static void make_lut(void)
{
    static const uint8_t level[4] = {0, 85, 170, 255};
    for (int v = 0; v < 64; v++)
        s_lut[v] = 0xFF000000u | (uint32_t)level[v & 3] << 16 | (uint32_t)level[(v >> 2) & 3] << 8 | level[(v >> 4) & 3];
}

// ---------------------------------------------------------------- keyboard
// Default: like a VNC viewer, the characters typed with the host's keyboard layout are passed
// on (an AZERTY keyboard types what it shows), shortcuts and special keys by position.

static bool s_raw_keys;
static uint8_t s_held[SDL_NUM_SCANCODES];   // usage pressed for each host scancode
static int s_pending = -1;                  // printable key waiting for its SDL_TEXTINPUT

static bool is_printable_scancode(SDL_Scancode sc)
{
    return (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_0) || (sc >= SDL_SCANCODE_MINUS && sc <= SDL_SCANCODE_SLASH) ||
           sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_NONUSBACKSLASH;
}

static void key(const SDL_KeyboardEvent *k)
{
    SDL_Scancode sc = k->keysym.scancode;
    if (k->repeat) return;    // the OS repeats keys itself
    if (!k->state) {
        if (s_pending == (int)sc) s_pending = -1;
        if (s_held[sc]) { input_key(s_held[sc], false); s_held[sc] = 0; }
        return;
    }
    if (s_raw_keys) {
        if (sc < 0xE8) { input_key(sc, true); s_held[sc] = sc; }
        return;
    }
    if (sc == SDL_SCANCODE_RALT) return;   // AltGr: already applied to the characters typed
    if (sc == SDL_SCANCODE_CAPSLOCK) return;
    SDL_Keymod m = SDL_GetModState();
    if (is_printable_scancode(sc)) {
        if (m & (KMOD_CTRL | KMOD_LALT | KMOD_GUI)) {
            // a shortcut: no text comes, use the key's character on the host layout (Ctrl+Z on
            // AZERTY is the key labelled Z)
            SDL_Keycode kc = k->keysym.sym;
            uint8_t u;
            bool shift;
            if (kc < 128 && input_char_to_usage((char)kc, &u, &shift)) { input_key(u, true); s_held[sc] = u; }
            return;
        }
        s_pending = sc;     // the character arrives in SDL_TEXTINPUT
        return;
    }
    if (sc < 0xE8) { input_key(sc, true); s_held[sc] = sc; }
}

static void text(const char *t)
{
    for (; *t; t++) {
        if ((unsigned char)*t >= 0x80) continue;   // the OS has ASCII only
        uint8_t u = input_char_down(*t);
        if (!u) continue;
        if (s_pending >= 0 && !t[1]) { s_held[s_pending] = u; s_pending = -1; }
        else input_key(u, false);
    }
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
    host_argv = argv;
    int scale = 2;
    bool headless = false;
    snprintf(host_disk, sizeof(host_disk), "%s", BEVEL_DEFAULT_DISK);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) snprintf(host_disk, sizeof(host_disk), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--serial-port") && i + 1 < argc) host_serial_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--raw-keys")) s_raw_keys = true;
        else if (!strcmp(argv[i], "--quiet")) host_echo_serial = false;
        else usage();
    }
    if (scale < 1 || scale > 4) scale = 2;
    if (host_disk[0] != '/') {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd))) { char t[1024]; snprintf(t, sizeof(t), "%s/%s", cwd, host_disk); snprintf(host_disk, sizeof(host_disk), "%s", t); }
    }
    signal(SIGPIPE, SIG_IGN);
    make_lut();

    if (headless) {
        app_main();
        for (;;) pause();
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("Bevel OS (simulator)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       VGA_W * scale, VGA_H * scale, SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(ren, VGA_W, VGA_H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VGA_W, VGA_H);
    SDL_ShowCursor(SDL_DISABLE);    // the OS draws its own pointer
    SDL_StartTextInput();

    app_main();

    static uint8_t frame[VGA_W * VGA_H];
    uint8_t buttons = 0;
    for (;;) {
        SDL_Event e;
        for (int got = SDL_WaitEventTimeout(&e, 10); got; got = SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: SDL_Quit(); exit(0);
            case SDL_KEYDOWN: case SDL_KEYUP: key(&e.key); break;
            case SDL_TEXTINPUT: text(e.text.text); break;
            case SDL_MOUSEMOTION: input_mouse_abs(e.motion.x, e.motion.y); break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                uint8_t b = e.button.button == SDL_BUTTON_LEFT ? BTN_LEFT : e.button.button == SDL_BUTTON_RIGHT ? BTN_RIGHT
                          : e.button.button == SDL_BUTTON_MIDDLE ? BTN_MID : 0;
                if (!b) break;
                input_mouse_abs(e.button.x, e.button.y);
                buttons = e.type == SDL_MOUSEBUTTONDOWN ? buttons | b : buttons & ~b;
                input_mouse_buttons(buttons);
                break;
            }
            case SDL_MOUSEWHEEL: if (e.wheel.y) input_wheel(e.wheel.y > 0 ? 1 : -1); break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    // keys released while the window was in the background must not stay down
                    for (int i = 0; i < SDL_NUM_SCANCODES; i++) if (s_held[i]) { input_key(s_held[i], false); s_held[i] = 0; }
                }
                break;
            }
        }
        const uint8_t *fb = vga_fb();
        if (fb) {
            memcpy(frame, fb, sizeof(frame));
            vga_cursor_overlay(frame);
            void *pixels;
            int pitch;
            if (SDL_LockTexture(tex, NULL, &pixels, &pitch) == 0) {
                for (int y = 0; y < VGA_H; y++) {
                    uint32_t *d = (uint32_t *)((uint8_t *)pixels + y * pitch);
                    const uint8_t *s = frame + y * VGA_W;
                    for (int x = 0; x < VGA_W; x++) d[x] = s_lut[s[x] & 63];
                }
                SDL_UnlockTexture(tex);
            }
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);   // paced by vsync, or by the event wait above
    }
}
