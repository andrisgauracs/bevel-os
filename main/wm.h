// Window manager and widget toolkit for Bevel OS.
// Everything here runs in the GUI task. Other tasks reach it through ev_call()/ev_call_sync().
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"
#include "input.h"
#include "theme.h"

typedef struct window window_t;
typedef struct widget widget_t;

// ------------------------------------------------------------------ metrics
#define TASKBAR_H   28
#define TITLE_H     18
#define MENUBAR_H   19
#define BORDER_W    4       // outer frame thickness
#define SCROLL_W    16
#define DESK_H      (SCREEN_H - TASKBAR_H)

// ------------------------------------------------------------------ menus
#define MI_DISABLED 1
#define MI_CHECKED  2
#define MI_RADIO    4
typedef struct menu_item {
    const char *label;              // "&Open...\tCtrl+O"; "-" is a separator; NULL ends the list
    int cmd;                        // command id sent to the handler (0 for submenu parents)
    const struct menu_item *sub;    // submenu, or NULL
    const bitmap_t *icon;           // optional 16x16
    uint8_t flags;
} menu_item_t;

typedef void (*menu_cb_t)(void *ctx, int cmd);
// Opens a popup menu at screen position (x, y). If 'above' is set the menu grows upwards from y.
void menu_popup(const menu_item_t *items, int x, int y, bool above, menu_cb_t cb, void *ctx);
void menu_close_all(void);
bool menu_is_open(void);

// ------------------------------------------------------------------ windows
#define WIN_RESIZABLE   0x0001
#define WIN_DIALOG      0x0002   // no minimise/maximise, Esc closes, not in the taskbar
#define WIN_NO_CLOSE    0x0004
#define WIN_MINIMIZED   0x0010
#define WIN_MAXIMIZED   0x0020
#define WIN_HIDDEN      0x0040
#define WIN_DEAD        0x0080   // closed, waiting to be freed
#define WIN_HUNG        0x0100   // app not responding (shown in the title)

struct window {
    window_t *prev, *next;          // z-order, next = above
    rect_t r;                       // frame rectangle on screen
    rect_t restore;                 // frame before maximise
    uint16_t flags;
    int16_t min_w, min_h;
    char title[64];
    const bitmap_t *icon;           // 16x16
    window_t *owner;                // window this dialog is modal to (it is disabled meanwhile)
    int modal_children;
    widget_t *widgets, *widgets_tail;
    widget_t *focus;
    widget_t *def_btn, *cancel_btn;
    const menu_item_t *menubar;     // top-level items with submenus, or NULL
    int task_id;                    // owning task for the task list (0 = system)
    void *user;

    // handlers (all optional)
    bool (*on_close)(window_t *w);                      // return false to veto
    void (*on_destroy)(window_t *w);
    void (*on_resize)(window_t *w);                     // client size changed: lay out widgets
    bool (*on_key)(window_t *w, const event_t *e);      // accelerators, before the focused widget
    void (*on_command)(window_t *w, int cmd);           // menu commands
    int  (*on_menu_state)(window_t *w, int cmd);        // extra MI_* flags for a menu item
    void (*on_activate)(window_t *w, bool active);
    int dlg_result;                 // for modal loops
    bool dlg_done;
};

window_t *wm_create(const char *title, int w, int h, uint16_t flags);  // client size; centred, hidden until wm_show
void wm_set_menubar(window_t *w, const menu_item_t *bar);  // before wm_show; keeps the client size
void wm_show(window_t *w);
void wm_close(window_t *w);            // asks on_close, then destroys
void wm_destroy(window_t *w);          // unconditional
void wm_activate(window_t *w);         // raise + focus (restores if minimised); NULL = desktop
void wm_minimize(window_t *w);
void wm_maximize(window_t *w, bool on);
void wm_move(window_t *w, int x, int y);
void wm_resize(window_t *w, int fw, int fh);
void wm_set_title(window_t *w, const char *title);
void wm_invalidate(window_t *w);
void wm_client_rect(const window_t *w, rect_t *out);   // screen coordinates
int  wm_client_w(const window_t *w);
int  wm_client_h(const window_t *w);
window_t *wm_active(void);
window_t *wm_top(void);                // topmost window (iterate downwards with ->prev)
bool wm_valid(window_t *w);
int  wm_run_modal(window_t *dlg);      // shows dlg and pumps events until wm_end_modal; returns result
void wm_end_modal(window_t *dlg, int result);
void wm_window_menu(window_t *w);      // Alt+Space menu
void wm_start_move(window_t *w, bool size);  // keyboard move / size mode

// The loop
void wm_init(void);
void wm_pump(int max_wait_ms);         // handle pending events, timers, repaint
void wm_run(void);                     // forever
void wm_set_cursor(const bitmap_t *c);
bool wm_alt_tapped(const event_t *e);  // e is the release of an Alt press with nothing in between
void wm_busy(bool on);                 // hourglass pointer
uint32_t wm_fps(void);                 // compositor flushes in the last second

// Timers (GUI task)
typedef void (*timer_cb_t)(void *arg);
int  timer_add(int period_ms, bool repeat, timer_cb_t cb, void *arg);
void timer_del(int id);

// Clipboard (text)
void clip_set(const char *s, int len);
const char *clip_get(int *len);

// ------------------------------------------------------------------ widgets
#define WF_HIDDEN     0x0001
#define WF_DISABLED   0x0002
#define WF_TABSTOP    0x0004
#define WF_IBEAM      0x0008   // show the text cursor over it
#define WF_WANT_ENTER 0x0010   // Enter goes to the widget, not the default button
#define WF_WANT_TAB   0x0020
#define WF_WANT_CHARS 0x0040   // plain letters are text here, not mnemonics
#define WF_CARET      0x0080   // repaint on caret blink
#define WF_WANT_ARROWS 0x0100
#define WF_WANT_ESC   0x0200
#define WF_WANT_KEYUP 0x0400   // also receives key releases (games)
#define WF_WANT_PINCH 0x0800   // receives EV_PINCH: mouse(type, x, y, btn = scale, clicks = PINCH_*)
#define WF_TOUCH_SCROLL 0x1000 // a finger dragged on it scrolls it: receives EV_TOUCH_SCROLL

// Notification codes for on_action
enum { UI_CLICK = 1, UI_CHANGE, UI_SELECT, UI_ACTIVATE, UI_CONTEXT };

typedef struct widget_class {
    const char *name;
    void (*paint)(widget_t *w, gfx_t *g);                       // origin = widget top-left
    bool (*key)(widget_t *w, const event_t *e);                 // key down / repeat
    void (*mouse)(widget_t *w, int type, int x, int y, int btn, int clicks); // widget coordinates
    void (*focus)(widget_t *w, bool gained);
    void (*destroy)(widget_t *w);
} widget_class_t;

struct widget {
    const widget_class_t *cls;
    window_t *win;
    widget_t *next, *prev;
    rect_t r;                       // in client coordinates
    uint16_t flags;
    int id;
    char *text;
    void (*on_action)(widget_t *w, int code);
    void *user;
};

void *ui_new(window_t *win, const widget_class_t *cls, size_t size, int x, int y, int w, int h);
void ui_delete(widget_t *w);
void ui_invalidate(widget_t *w);
void ui_set_text(widget_t *w, const char *text);
void ui_set_rect(widget_t *w, int x, int y, int ww, int hh);
void ui_focus(widget_t *w);
void ui_enable(widget_t *w, bool on);
void ui_show(widget_t *w, bool on);
bool ui_focused(const widget_t *w);     // has focus and its window is active
void ui_capture(widget_t *w);           // receive mouse moves/up until the button is released
int  ui_touch_steps(int dy, int step);  // EV_TOUCH_SCROLL: whole rows of `step` pixels, keeping the rest
void ui_notify(widget_t *w, int code);
bool ui_caret_on(void);                 // caret blink phase

widget_t *ui_label(window_t *win, int x, int y, int w, int h, const char *text);
widget_t *ui_button(window_t *win, int x, int y, int w, int h, const char *text, void (*on_click)(widget_t *, int));
void ui_button_icon(widget_t *b, const bitmap_t *icon);   // shown left of the text, or alone with text ""
widget_t *ui_checkbox(window_t *win, int x, int y, int w, const char *text, bool checked);
widget_t *ui_radio(window_t *win, int x, int y, int w, const char *text, int group, bool checked);
bool ui_checked(const widget_t *w);
void ui_set_checked(widget_t *w, bool on);
int  ui_radio_selected(window_t *win, int group);   // index within the group, -1 if none
widget_t *ui_groupbox(window_t *win, int x, int y, int w, int h, const char *text);
widget_t *ui_icon(window_t *win, int x, int y, const bitmap_t *bmp);

widget_t *ui_textfield(window_t *win, int x, int y, int w, const char *text, int max_len);
const char *ui_textfield_get(widget_t *w);
void ui_textfield_mask(widget_t *w, bool on);   // show bullets (passwords)
void ui_textfield_set(widget_t *w, const char *text);
void ui_textfield_select_all(widget_t *w);
bool file_dialog(window_t *owner, bool save, const char *title, char *path, int path_size); // path: in = start, out = choice

widget_t *ui_slider(window_t *win, int x, int y, int w, int min, int max, int value);
int  ui_slider_get(const widget_t *w);
void ui_slider_set(widget_t *w, int v);

// List view: rows fetched on demand.
typedef struct {
    const char *text;               // column 0
    const char *col[3];             // further columns (may be NULL)
    const bitmap_t *icon;           // 16x16 or NULL
} lv_item_t;
typedef void (*lv_get_fn)(widget_t *lv, int index, lv_item_t *out);
widget_t *ui_listview(window_t *win, int x, int y, int w, int h, lv_get_fn get);
void ui_listview_columns(widget_t *lv, int n, const char *const *titles, const int *widths); // header row
void ui_listview_set_count(widget_t *lv, int count);
int  ui_listview_count(const widget_t *lv);
int  ui_listview_sel(const widget_t *lv);
void ui_listview_select(widget_t *lv, int index);       // also scrolls it into view
void ui_listview_context_pos(widget_t *lv, int *sx, int *sy); // screen position for a context menu

// Drop-down list: a field showing the chosen item; the list opens over everything on a click,
// Space, F4 or Alt+Down. Arrows and letters change the choice while it is closed. UI_SELECT on change.
widget_t *ui_dropdown(window_t *win, int x, int y, int w, lv_get_fn get);   // 22 pixels high
void ui_dropdown_set_count(widget_t *dd, int count);
int  ui_dropdown_sel(const widget_t *dd);
void ui_dropdown_select(widget_t *dd, int index);       // no UI_SELECT

// Canvas: a custom-drawn, optionally focusable area.
typedef struct {
    void (*paint)(widget_t *w, gfx_t *g);
    bool (*key)(widget_t *w, const event_t *e);
    void (*mouse)(widget_t *w, int type, int x, int y, int btn, int clicks);
} canvas_ops_t;
widget_t *ui_canvas(window_t *win, int x, int y, int w, int h, const canvas_ops_t *ops, bool focusable);

// Scrollbar helper (used by list view, text area, ...). Geometry is in the caller's coordinates.
typedef struct { int pos, page, total; int drag_off; int8_t pressed; } scroll_t; // pressed: 0 none, 1 up, 2 down, 3 thumb
void scroll_paint(gfx_t *g, const rect_t *r, const scroll_t *s, bool horizontal);
// Returns true if pos changed. type is EV_MOUSE_DOWN / EV_MOUSE_MOVE / EV_MOUSE_UP.
bool scroll_mouse(scroll_t *s, const rect_t *r, bool horizontal, int type, int x, int y, int line);
void scroll_clamp(scroll_t *s);

// Theme drawing helpers
void ui_draw_raised(gfx_t *g, int x, int y, int w, int h);     // button up (2px)
void ui_draw_pressed(gfx_t *g, int x, int y, int w, int h);    // button down
void ui_draw_sunken(gfx_t *g, int x, int y, int w, int h);     // field / list frame (2px)
void ui_draw_groove(gfx_t *g, int x, int y, int w, int h);     // etched frame
void ui_draw_button(gfx_t *g, int x, int y, int w, int h, const char *label, const bitmap_t *icon,
                    bool pressed, bool focused, bool is_default, bool disabled);
bool ui_show_mnemonics(void);

// ------------------------------------------------------------------ dialogs
enum { MB_OK = 1, MB_OKCANCEL, MB_YESNO, MB_YESNOCANCEL };
enum { ID_OK = 1, ID_CANCEL = 2, ID_YES = 3, ID_NO = 4 };
enum { MB_INFO = 0, MB_WARN, MB_ERROR, MB_ASK };
int  msgbox(window_t *owner, const char *title, const char *text, int icon, int buttons);
// Text prompt; returns true on OK. buf holds the initial text and receives the result.
bool inputbox(window_t *owner, const char *title, const char *prompt, char *buf, int buf_size);
bool inputbox_password(window_t *owner, const char *title, const char *prompt, char *buf, int buf_size);

// ------------------------------------------------------------------ shell (desktop, taskbar)
void shell_init(void);
void shell_paint_desktop(gfx_t *g, const rect_t *area);
void shell_paint_taskbar(gfx_t *g, const rect_t *area);
void shell_paint_overlay(gfx_t *g, const rect_t *area);       // Alt+Tab switcher
bool shell_key(const event_t *e);                              // global keys; true if consumed
bool shell_focus_key(const event_t *e);                        // desktop / taskbar focus
void shell_mouse(int type, int x, int y, int btn, int clicks); // desktop + taskbar
bool shell_volume_mouse(int type, int x, int y, int btn);      // the volume popup; true if consumed
void shell_windows_changed(void);                              // taskbar buttons need a repaint
void shell_focus_desktop(void);
bool shell_has_focus(void);                                    // desktop or taskbar has the keyboard
void shell_open_start_menu(void);
void shell_shutdown_dialog(void);
extern uint8_t shell_desktop_color;
void wallpaper_paint(gfx_t *g, pixel_t background);            // boards with BOARD_HAS_WALLPAPER

// Menu internals used by wm.c
void menu_paint(gfx_t *g, const rect_t *area);
bool menu_key(const event_t *e);
bool menu_mouse(int type, int x, int y, int btn);              // true if consumed
void menubar_paint(window_t *w, gfx_t *g, const rect_t *bar);  // bar in screen coordinates
bool menubar_key(window_t *w, const event_t *e);               // Alt / F10 / Alt+letter
void menubar_click(window_t *w, int x, int y);
window_t *menubar_owner(void);                                 // window whose menu bar is active, or NULL
int  menubar_active_index(void);

// Open drop-down list (ui.c), an overlay like the menus; used by wm.c
bool ui_popup_is_open(void);
void ui_popup_close(void);
void ui_popup_paint(gfx_t *g, const rect_t *area);
bool ui_popup_key(const event_t *e);                           // true if consumed
bool ui_popup_mouse(int type, int x, int y, int btn);          // true if consumed
bool ui_popup_scrolls_at(int x, int y);                        // a finger dragged here scrolls the list
void ui_popup_touch_scroll(int dy);
