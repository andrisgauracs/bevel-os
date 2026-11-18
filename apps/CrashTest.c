// CrashTest: misbehaves on purpose, to show that the desktop survives a bad program.
#include "bevel.h"

// Test 2 is the fault that each CPU really raises: a division by zero traps on the ESP32-S3, but the
// RISC-V in the P4 does not trap on it at all. tools/build_apps.sh sets BV_ARCH_RISCV per target.
#ifndef BV_ARCH_RISCV
#define BV_ARCH_RISCV 0
#endif

static volatile int zero;

int app_main(const bevel_api_t *os, const char *arg)
{
    if (os->win_open("Crash Test", 300, 120) != 0) return 1;
    os->clear(BV_RGB(2, 2, 2));
    os->text(10, 10, "This program breaks on purpose.", BV_FONT_BOLD, BV_RGB(0, 0, 0));
    os->text(10, 34, "1   write to an invalid address", BV_FONT_UI, BV_RGB(0, 0, 0));
    os->text(10, 50, BV_ARCH_RISCV ? "2   jump to an invalid address" : "2   divide by zero", BV_FONT_UI, BV_RGB(0, 0, 0));
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
#if BV_ARCH_RISCV
        if (e.ch == '2') { void (*volatile nowhere)(void) = (void (*)(void))(zero + 16); nowhere(); }
#else
        if (e.ch == '2') { volatile int r = 10 / zero; (void)r; }
#endif
        if (e.ch == '3') for (;;) { }
    }
}
