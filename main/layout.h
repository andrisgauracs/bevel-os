// Keyboard layouts: what a physical key prints. A key is a USB HID usage code, and each key
// has four states - plain, Shift, AltGr, AltGr+Shift - because that is how keyboards are
// printed. The tables come from tools/layouts/ (the X keyboard config) via tools/gen_layouts.py.
//
// A state can also be a dead key: it prints nothing on its own and arms an accent, which
// composes with the next letter (French ^ then e makes ê). Pressing a dead key and then
// Space, or a letter the accent does not fit, prints the accent on its own.
#pragma once
#include <stdint.h>

enum { DEAD_NONE = 0, DEAD_CIRCUMFLEX, DEAD_GRAVE, DEAD_ACUTE, DEAD_DIAERESIS, DEAD_TILDE };

enum { LAYOUT_USAGE_MIN = 4, LAYOUT_USAGE_MAX = 100 };  // HID usages the tables cover

#define layout_count 7

extern const uint16_t (*const layout_cp[layout_count])[4];    // code point for usage+state, 0 = nothing
extern const uint16_t (*const layout_dead[layout_count])[4];  // DEAD_* for usage+state
extern const char *const layout_name[layout_count];           // shown in the Settings list

// the state of a key: bit0 Shift, bit1 AltGr (the right Alt key)
#define L_STATE_SHIFT 1
#define L_STATE_ALTGR 2
static inline int layout_state(int shift, int altgr) { return (shift ? 1 : 0) | (altgr ? 2 : 0); }

int input_layout_get(void);                       // the layout the OS types with
void input_layout_set(int layout);                // apply it (drops a pending dead key)
const char *input_utf8(uint16_t code);            // the bytes for one code point (static table)
