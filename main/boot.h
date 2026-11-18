// Boot splash and progress bar.
//
// The panel wakes up long before the desktop is ready to answer the mouse: the drive has to be
// mounted (and after a firmware update its bundled files rewritten, which takes seconds), the
// programs on it scanned, the touch panel and the audio started, and a USB keyboard or mouse has to
// be enumerated before anything on the screen can be clicked. The desktop's own pixels only reach the
// framebuffer at the first wm_pump(), so from boot_init() until that moment this screen owns the
// framebuffer and shows it straight away: invalidations queued in between stay invisible, because
// comp_flush() only runs in wm_pump().
#pragma once
#include <stdbool.h>

// Everything here runs in the GUI task, in the window between disp_init() and the first wm_pump().

// The stages, in the order the boot sequence in main.c walks through them. Each one owns one ninth
// of the bar. The two board-dependent stages (touch, sound) are always reported: on a board without
// that hardware the stage is done at once, so the bar only ticks forward.
enum {
    BOOT_STAGE_DRIVE = 0,   // mount the drive, restore the bundled files
    BOOT_STAGE_SOUND,       // start the audio: the system sounds are synthesised here, in the background
    BOOT_STAGE_SETTINGS,
    BOOT_STAGE_CLOCK,
    BOOT_STAGE_DESKTOP,
    BOOT_STAGE_APPS,
    BOOT_STAGE_TOUCH,
    BOOT_STAGE_NET,
    BOOT_STAGE_INPUT,       // waiting for a keyboard or a mouse: the last part of the bar
    BOOT_STAGE_COUNT
};

void boot_init(void);                            // right after disp_init(): the panel goes black, the splash appears
void boot_stage(int stage);                      // this stage is starting: the bar advances to its start
void boot_sub(int stage, int index, int total, const char *detail); // work inside a stage (index < total)
void boot_wait_input(int timeout_ms);            // hold back the last ninth of the bar until an input device answers
void boot_done(void);                            // called after the desktop has been painted: logs the timings
int  boot_elapsed_ms(void);                      // when the splash left the screen, ms after reset (-1 = still up)
int  boot_ready_ms(void);                        // when the desktop took over, ms after reset
