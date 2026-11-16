// CrashTest: misbehaves on purpose, to show that the desktop survives a bad program.
#include "bevel.h"

static volatile int zero;

int app_main(const bevel_api_t *os, const char *arg)
{
    if (os->win_open("Crash Test", 300, 120) != 0) return 1;
    os->clear(BV_RGB(2, 2, 2));
    os->text(10, 10, "This program breaks on purpose.", BV_FONT_BOLD, BV_RGB(0, 0, 0));
    os->text(10, 34, "1   write to an invalid address", BV_FONT_UI, BV_RGB(0, 0, 0));
    os->text(10, 50, "2   jump to an invalid address", BV_FONT_UI, BV_RGB(0, 0, 0));
    os->text(10, 66, "3   hang in an endless loop", BV_FONT_UI, BV_RGB(0, 0, 0));
    os->text(10, 82, "Esc close normally", BV_FONT_UI, BV_RGB(0, 0, 0));
    os->flush();
    for (;;) {
        bv_event_t e;
        if (!os->event(&e, -1)) continue;
        if (e.type == BV_CLOSE) return 0;
        if (e.type != BV_KEY_DOWN) continue;
        if (e.key == BV_K_ESC) return 0;
        if (e.ch == '1') *(volatile int *)4 = 42;
        if (e.ch == '2') { void (*volatile nowhere)(void) = (void (*)(void))(zero + 16); nowhere(); }   // (a division by zero does not trap on RISC-V)
        if (e.ch == '3') for (;;) { }
    }
}
