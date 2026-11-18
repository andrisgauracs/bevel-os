// Input events and the GUI event queue. Keys are USB HID usage codes (US layout).
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum {
    EV_NONE = 0,
    EV_KEY,          // key, ch, mods, down, repeat
    EV_MOUSE_MOVE,   // x, y (coalesced: read the latest position)
    EV_MOUSE_DOWN,   // button, x, y
    EV_MOUSE_UP,
    EV_WHEEL,        // x = delta
    EV_CALL,         // run fn(arg) in the GUI task
    EV_PINCH,        // x, y = between the fingers, scale, button = PINCH_*
    EV_TOUCH,        // one finger: x, y, button = TOUCH_* (window manager, which knows what is under it)
    EV_TOUCH_SCROLL, // to a WF_TOUCH_SCROLL widget's mouse function: btn = pixels the finger moved down
};

// Two fingers on the touchscreen. scale is their distance against where the gesture began, 256 = same.
enum { PINCH_BEGIN, PINCH_MOVE, PINCH_END };
enum { TOUCH_TAP, TOUCH_LONG, TOUCH_DRAG_BEGIN, TOUCH_DRAG_MOVE, TOUCH_DRAG_END };

#define MOD_CTRL  1
#define MOD_SHIFT 2
#define MOD_ALT   4
#define MOD_GUI   8
#define MOD_RALT  16   // the right Alt key: AltGr, which types layout characters, not shortcuts

#define BTN_LEFT  1
#define BTN_RIGHT 2
#define BTN_MID   4

enum {
    K_A = 0x04, K_B, K_C, K_D, K_E, K_F, K_G, K_H, K_I, K_J, K_K, K_L, K_M, K_N, K_O, K_P, K_Q, K_R,
    K_S, K_T, K_U, K_V, K_W, K_X, K_Y, K_Z,
    K_1 = 0x1E, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_0,
    K_ENTER = 0x28, K_ESC, K_BKSP, K_TAB, K_SPACE,
    K_F1 = 0x3A, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
    K_INS = 0x49, K_HOME, K_PGUP, K_DEL, K_END, K_PGDN, K_RIGHT, K_LEFT, K_DOWN, K_UP,
    K_KP_ENTER = 0x58,
    K_MENU = 0x65,
    K_LCTRL = 0xE0, K_LSHIFT, K_LALT, K_LGUI, K_RCTRL, K_RSHIFT, K_RALT, K_RGUI,
};

typedef struct {
    uint8_t type;
    uint8_t mods;
    uint8_t key;        // HID usage
    char ch;            // first byte of the text this key produces, or 0
    char chs[6];        // the text the key produces: UTF-8 bytes (1-2 per glyph), empty string = none
    uint8_t down;
    uint8_t repeat;
    uint8_t button;
    int16_t x, y;
    int16_t scale;      // EV_PINCH
    void (*fn)(void *);
    void *arg;
} event_t;

void input_init(void);
bool ev_post(const event_t *e);
bool ev_wait(event_t *e, int timeout_ms);
void ev_call(void (*fn)(void *), void *arg);       // asynchronous
void ev_call_sync(void (*fn)(void *), void *arg);  // returns after fn ran in the GUI task

// Raw input sources (USB host, serial injection). Safe from any task.
void input_key(uint8_t usage, bool down);
void input_kbd_report(uint8_t mod_bits, const uint8_t *keys, int nkeys); // boot-protocol style report
void input_mouse_rel(int dx, int dy);
void input_mouse_abs(int x, int y);               // screen pixels
void input_touch(int x, int y, bool down);        // touchscreen: a finger is the left button
void input_touch_right_click(int x, int y);       // touchscreen: a long touch
void input_pinch(int x, int y, int scale, int phase); // touchscreen: two fingers (EV_PINCH)
void input_touch_gesture(int phase, int x, int y); // touchscreen: one finger (EV_TOUCH)
void input_touch_latest(int *x, int *y);          // where the finger is now, for TOUCH_DRAG_MOVE
void input_touch_hide_pointer(void);
bool input_touching(void);                        // the last pointer input came from the touchscreen
void input_mouse_buttons(uint8_t mask);           // BTN_* bitmask, full state
void input_wheel(int delta);
void input_release_all(void);                     // device unplugged

// State
void input_mouse_pos(int *x, int *y);
uint8_t input_mods(void);
uint8_t input_buttons(void);
bool input_mouse_seen(void);                      // true once any mouse activity happened
void input_pointer_frozen(bool frozen);           // drop pointer input (boot splash owns the screen)
void input_pointer_busy(bool busy);               // show the pointer (the hourglass) even after touch hid it

// Settings
extern int input_mouse_speed;   // 1..10, 5 = 1:1
extern int input_repeat_delay;  // ms before repeating
extern int input_repeat_rate;   // repeats per second

// Called by the GUI loop: posts a repeat key event when one is due; returns ms until the next one (or -1).
int input_repeat_poll(void);

// US layout helpers
char input_usage_to_char(uint8_t usage, uint8_t mods);
bool input_char_to_usage(char c, uint8_t *usage, bool *shift);
// the key that prints a code point in the current layout (state: bit0 Shift, bit1 AltGr)
bool input_key_find(uint16_t code, uint8_t *usage, int *state);
void input_key_text_test(uint8_t usage, uint8_t mods, char *out, int cap);  // layout check for the serial console
