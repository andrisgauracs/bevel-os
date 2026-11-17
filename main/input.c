// Input: turns raw key/mouse/touch data into events for the GUI task. The mouse pointer itself is
// moved right here (the display draws it over the picture), so it tracks the mouse even while the
// GUI task is busy.
#include <string.h>
#include "input.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

static QueueHandle_t s_q;
static volatile int s_mx = SCREEN_W / 2, s_my = SCREEN_H / 2;
static int s_frac_x, s_frac_y;
static volatile uint8_t s_mods, s_buttons;
static volatile bool s_move_pending, s_mouse_seen;
static uint8_t s_keys_down[32];      // bitmap of usages 0..255
static uint8_t s_prev_report[6];
static uint8_t s_prev_mod_bits;

static volatile uint8_t s_rep_key;   // key being auto-repeated (0 = none)
static volatile int64_t s_rep_next;  // time of next repeat, us

int input_mouse_speed = 5;
int input_repeat_delay = 400;
int input_repeat_rate = 25;

void input_init(void) { s_q = xQueueCreate(64, sizeof(event_t)); }

bool ev_post(const event_t *e) { return xQueueSend(s_q, e, 0) == pdTRUE; }

bool ev_wait(event_t *e, int timeout_ms)
{
    return xQueueReceive(s_q, e, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ev_call(void (*fn)(void *), void *arg)
{
    event_t e = {.type = EV_CALL, .fn = fn, .arg = arg};
    xQueueSend(s_q, &e, portMAX_DELAY);
}

typedef struct { void (*fn)(void *); void *arg; SemaphoreHandle_t done; } sync_call_t;
static void sync_tramp(void *p)
{
    sync_call_t *c = p;
    if (c->fn) c->fn(c->arg);
    xSemaphoreGive(c->done);
}
void ev_call_sync(void (*fn)(void *), void *arg)
{
    sync_call_t c = {fn, arg, xSemaphoreCreateBinary()};
    ev_call(sync_tramp, &c);
    xSemaphoreTake(c.done, portMAX_DELAY);
    vSemaphoreDelete(c.done);
}

// ---------------------------------------------------------------- keyboard

static const char s_plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char s_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

static bool s_caps;

char input_usage_to_char(uint8_t usage, uint8_t mods)
{
    if (s_caps && usage >= K_A && usage <= K_Z) mods ^= MOD_SHIFT;
    if (usage == K_KP_ENTER) return '\n';
    if (usage >= 0x54 && usage <= 0x63) return "/*-+\n1234567890."[usage - 0x54]; // keypad
    if (usage < 0x04 || usage > 0x38) return 0;
    char c = (mods & MOD_SHIFT) ? s_shift[usage - 4] : s_plain[usage - 4];
    if (c == '\x1b' || c == '\b') return 0;
    return c;
}

bool input_char_to_usage(char c, uint8_t *usage, bool *shift)
{
    if (c == '\r') c = '\n';
    for (int i = 0; i < (int)sizeof(s_plain) - 1; i++) {
        if (s_plain[i] == c && c != '#') { *usage = i + 4; *shift = false; return true; }
    }
    for (int i = 0; i < (int)sizeof(s_shift) - 1; i++) {
        if (s_shift[i] == c && !(c == '~' && i + 4 == 0x32)) { *usage = i + 4; *shift = true; return true; }
    }
    return false;
}

static uint8_t mod_of(uint8_t usage)
{
    switch (usage) {
    case K_LCTRL: case K_RCTRL: return MOD_CTRL;
    case K_LSHIFT: case K_RSHIFT: return MOD_SHIFT;
    case K_LALT: case K_RALT: return MOD_ALT;
    case K_LGUI: case K_RGUI: return MOD_GUI;
    }
    return 0;
}

void input_key(uint8_t usage, bool down)
{
    if (usage < 4) return;
    bool was = s_keys_down[usage >> 3] & (1 << (usage & 7));
    if (down) s_keys_down[usage >> 3] |= 1 << (usage & 7);
    else s_keys_down[usage >> 3] &= ~(1 << (usage & 7));
    if (was == down && !down) return;
    if (usage == 0x39 && down && !was) s_caps = !s_caps;   // Caps Lock
    uint8_t m = mod_of(usage);
    if (m) {
        if (down) s_mods |= m; else s_mods &= ~m;
    }
    event_t e = {.type = EV_KEY, .key = usage, .mods = s_mods, .down = down};
    e.ch = input_usage_to_char(usage, s_mods);
    if ((s_mods & (MOD_CTRL | MOD_ALT | MOD_GUI)) && e.ch) {
        // keep the base letter so shortcuts can match on it
        e.ch = input_usage_to_char(usage, 0);
    }
    ev_post(&e);
    if (down && !m) {
        s_rep_next = esp_timer_get_time() + (int64_t)input_repeat_delay * 1000;
        s_rep_key = usage;
    } else if (!down && usage == s_rep_key) {
        s_rep_key = 0;
    }
}

int input_repeat_poll(void)
{
    uint8_t k = s_rep_key;
    if (!k) return -1;
    int64_t now = esp_timer_get_time();
    int64_t due = s_rep_next;
    if (now >= due) {
        event_t e = {.type = EV_KEY, .key = k, .mods = s_mods, .down = 1, .repeat = 1};
        e.ch = input_usage_to_char(k, (s_mods & (MOD_CTRL | MOD_ALT | MOD_GUI)) ? 0 : s_mods);
        ev_post(&e);
        int period = 1000000 / (input_repeat_rate > 0 ? input_repeat_rate : 1);
        s_rep_next = now + period;
        return period / 1000;
    }
    return (int)((due - now) / 1000) + 1;
}

void input_kbd_report(uint8_t mod_bits, const uint8_t *keys, int nkeys)
{
    if (nkeys > 6) nkeys = 6;
    // modifiers: bit0 LCtrl, 1 LShift, 2 LAlt, 3 LGui, 4 RCtrl, 5 RShift, 6 RAlt, 7 RGui
    uint8_t changed = mod_bits ^ s_prev_mod_bits;
    for (int i = 0; i < 8; i++) {
        if (changed & (1 << i)) input_key(K_LCTRL + i, mod_bits & (1 << i));
    }
    s_prev_mod_bits = mod_bits;
    if (nkeys > 0 && keys[0] == 1) return; // rollover error
    for (int i = 0; i < 6; i++) {
        uint8_t k = s_prev_report[i];
        if (k < 4) continue;
        bool still = false;
        for (int j = 0; j < nkeys; j++) if (keys[j] == k) still = true;
        if (!still) input_key(k, false);
    }
    for (int j = 0; j < nkeys; j++) {
        uint8_t k = keys[j];
        if (k < 4) continue;
        bool had = false;
        for (int i = 0; i < 6; i++) if (s_prev_report[i] == k) had = true;
        if (!had) input_key(k, true);
    }
    memset(s_prev_report, 0, 6);
    memcpy(s_prev_report, keys, nkeys);
}

void input_release_all(void)
{
    for (int u = 4; u < 256; u++) {
        if (s_keys_down[u >> 3] & (1 << (u & 7))) input_key(u, false);
    }
    memset(s_prev_report, 0, 6);
    s_prev_mod_bits = 0;
    s_rep_key = 0;
    if (s_buttons) input_mouse_buttons(0);
}

// ---------------------------------------------------------------- mouse

static void post_move(void)
{
    s_mouse_seen = true;
    disp_cursor_move(s_mx, s_my);
    if (!s_move_pending) {
        s_move_pending = true;
        event_t e = {.type = EV_MOUSE_MOVE};
        if (!ev_post(&e)) s_move_pending = false;
    }
}

// The pointer is hidden while the touchscreen is in use and comes back with the mouse.
static volatile bool s_touching, s_ptr_hidden;

static void pointer_wanted(bool on)
{
    if (s_ptr_hidden == !on) return;
    s_ptr_hidden = !on;
    disp_cursor_show(on);
}

// While the boot splash owns the screen the pointer is not drawn and there is nothing under it to
// click, so pointer input is dropped rather than queued: when the desktop appears, the cursor starts
// where the window manager put it - the middle of the screen - instead of wherever the last few
// seconds of waiting had dragged it. The reports still count as "a pointer is attached", which is
// exactly what the boot wait is watching for, so the splash is not held up by this.
static volatile bool s_ptr_frozen;

void input_pointer_frozen(bool frozen) { s_ptr_frozen = frozen; }

static void mouse_to(int x, int y)
{
    if (s_ptr_frozen) { s_mouse_seen = true; return; }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > SCREEN_W - 1) x = SCREEN_W - 1;
    if (y > SCREEN_H - 1) y = SCREEN_H - 1;
    if (x == s_mx && y == s_my) return;
    s_mx = x; s_my = y;
    post_move();
}

void input_mouse_abs(int x, int y)
{
    if (s_ptr_hidden) pointer_wanted(true);
    mouse_to(x, y);
}

void input_touch(int x, int y, bool down)
{
    if (down) {
        if (!s_touching) pointer_wanted(false);
        s_touching = true;
        mouse_to(x, y);
        if (!(s_buttons & BTN_LEFT)) input_mouse_buttons(s_buttons | BTN_LEFT);
    } else {
        s_touching = false;
        input_mouse_buttons(s_buttons & ~BTN_LEFT);
    }
}

bool input_touching(void) { return s_ptr_hidden; }

void input_mouse_rel(int dx, int dy)
{
    if (!dx && !dy) return;
    // speed 5 = 1:1; a little acceleration for fast movements
    int mag = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    int gain = input_mouse_speed * (mag > 12 ? 3 : mag > 5 ? 2 : 1); // in 1/5 pixel per count
    s_frac_x += dx * gain;
    s_frac_y += dy * gain;
    int mx = s_frac_x / 5, my = s_frac_y / 5;
    s_frac_x -= mx * 5; s_frac_y -= my * 5;
    if (s_ptr_hidden) pointer_wanted(true);
    mouse_to(s_mx + mx, s_my + my);
}

void input_mouse_buttons(uint8_t mask)
{
    uint8_t changed = mask ^ s_buttons;
    if (!changed) return;
    s_mouse_seen = true;
    if (s_ptr_frozen) return;   // a click at the splash lands on nothing, so it is not recorded
    for (int b = 1; b <= 4; b <<= 1) {
        if (!(changed & b)) continue;
        if (mask & b) s_buttons |= b; else s_buttons &= ~b;
        event_t e = {.type = (mask & b) ? EV_MOUSE_DOWN : EV_MOUSE_UP, .button = b, .mods = s_mods,
                     .x = s_mx, .y = s_my};
        ev_post(&e);
    }
}

void input_wheel(int delta)
{
    if (!delta || s_ptr_frozen) return;
    event_t e = {.type = EV_WHEEL, .x = delta, .mods = s_mods};
    ev_post(&e);
}

void input_mouse_pos(int *x, int *y)
{
    s_move_pending = false;
    *x = s_mx; *y = s_my;
}

uint8_t input_mods(void) { return s_mods; }
uint8_t input_buttons(void) { return s_buttons; }
bool input_mouse_seen(void) { return s_mouse_seen; }
