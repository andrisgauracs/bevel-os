// Small built-in apps: System information, Task list, Help, Benchmark, Widget gallery.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "display.h"
#include "comp.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "net.h"

// ================================================================ System information

static window_t *s_sys;
static int s_sys_timer;

static void sys_paint(widget_t *w, gfx_t *g)
{
    char line[96];
    int y = 0;
    gfx_bitmap(g, 4, 2, &icon32_logo);
    gfx_text(g, 48, 4, OS_NAME " " OS_VERSION, -1, FONT_UI_BOLD, C_TEXT);
    gfx_text(g, 48, 20, "A small desktop OS for the " OS_CHIP, -1, FONT_UI, C_TEXT);
    y = 46;
    ui_draw_groove(g, 0, y - 6, w->r.w, 2);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint32_t up = os_uptime_ms() / 1000;
    const char *labels[12];
    char vals[12][64];
    int n = 0;
    labels[n] = "Processor:"; snprintf(vals[n++], 64, OS_CHIP " rev %d.%d, %d cores at %d MHz", chip.revision / 100, chip.revision % 100, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    labels[n] = "Memory:"; snprintf(vals[n++], 64, "%u KB free of %u KB internal RAM",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
    labels[n] = "External memory:"; snprintf(vals[n++], 64, "%u KB free of %u KB PSRAM",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)(esp_psram_get_size() / 1024));
    labels[n] = "Flash:"; snprintf(vals[n++], 64, "%u MB", (unsigned)(flash >> 20));
    labels[n] = "Drive:"; os_disk_summary(vals[n++], 64);
    labels[n] = "SD card:"; os_sd_summary(vals[n++], 64);
    labels[n] = "Display:";
#if DISP_HAS_OVERLAYS
    snprintf(vals[n++], 64, "%d x %d DSI panel, 64 colours + overlays", SCREEN_W, SCREEN_H);
#else
    // no true-colour layers on the VGA scan-out: the 64 colours are all there is
    snprintf(vals[n++], 64, "%d x %d VGA monitor, 64 colours", SCREEN_W, SCREEN_H);
#endif
    labels[n] = "Network:"; net_summary(vals[n++], 64);
    labels[n] = "Uptime:"; snprintf(vals[n++], 64, "%u:%02u:%02u", (unsigned)(up / 3600), (unsigned)(up / 60 % 60), (unsigned)(up % 60));
    labels[n] = "Screen updates:"; snprintf(vals[n++], 64, "%u per second", (unsigned)wm_fps());
    for (int i = 0; i < n; i++, y += 16) {
        gfx_text(g, 4, y, labels[i], -1, FONT_UI, C_TEXT);
        gfx_text(g, 124, y, vals[i], -1, FONT_UI, C_NAVY);
    }
    (void)line;
}
static const canvas_ops_t sys_ops = {sys_paint, NULL, NULL};

static void sys_tick(void *arg)
{
    if (!s_sys) return;
    for (widget_t *w = s_sys->widgets; w; w = w->next) if (!w->text) ui_invalidate(w);
}
static void sys_destroy(window_t *w) { timer_del(s_sys_timer); s_sys = NULL; }
static void sys_ok(widget_t *w, int code) { wm_close(w->win); }

void app_system(const char *arg)
{
    if (s_sys) { wm_activate(s_sys); return; }
    window_t *w = wm_create("System", 400, 272, 0);
    w->icon = &icon16_computer;
    w->task_id = 0;
    ui_canvas(w, 12, 10, 376, 222, &sys_ops, false);
    widget_t *ok = ui_button(w, 310, 238, 78, 24, "OK", sys_ok);
    w->def_btn = ok; w->cancel_btn = ok;
    w->on_destroy = sys_destroy;
    s_sys = w;
    s_sys_timer = timer_add(1000, true, sys_tick, NULL);
    wm_show(w);
}

// ================================================================ Task list

static window_t *s_tl;
static widget_t *s_tl_list;
static int s_tl_timer;
static window_t *s_tl_wins[24];
static int s_tl_n;
static char s_tl_mem[24][16];

static void tl_refresh(void *arg)
{
    if (!s_tl) return;
    int n = 0;
    for (window_t *w = wm_top(); w && n < 24; w = w->prev) {
        if ((w->flags & (WIN_HIDDEN | WIN_DEAD)) || w->owner || w == s_tl) continue;
        s_tl_wins[n] = w;
        int kb = os_task_mem_kb(w->task_id);
        if (kb >= 0) snprintf(s_tl_mem[n], 16, "%d KB", kb); else strcpy(s_tl_mem[n], "(system)");
        n++;
    }
    s_tl_n = n;
    ui_listview_set_count(s_tl_list, n);
}

static void tl_get(widget_t *lv, int i, lv_item_t *it)
{
    window_t *w = s_tl_wins[i];
    it->text = w->title;
    it->icon = w->icon;
    it->col[0] = (w->flags & WIN_HUNG) ? "Not responding" : (w->flags & WIN_MINIMIZED) ? "Minimised" : "Running";
    it->col[1] = s_tl_mem[i];
}

static window_t *tl_selected(void)
{
    int i = ui_listview_sel(s_tl_list);
    if (i < 0 || i >= s_tl_n || !wm_valid(s_tl_wins[i])) return NULL;
    return s_tl_wins[i];
}

static void tl_switch(widget_t *b, int code)
{
    window_t *w = tl_selected();
    if (w) { wm_close(s_tl); wm_activate(w); }
}

static void tl_end(widget_t *b, int code)
{
    window_t *w = tl_selected();
    if (!w) return;
    if (!os_task_kill(w->task_id)) wm_destroy(w);   // system windows are simply destroyed
    tl_refresh(NULL);
    ui_focus(s_tl_list);
}

static void tl_close(widget_t *b, int code) { wm_close(b->win); }
static void tl_list_action(widget_t *lv, int code) { if (code == UI_ACTIVATE) tl_switch(lv, 0); }
static void tl_destroy(window_t *w) { timer_del(s_tl_timer); s_tl = NULL; }

void app_tasks(const char *arg)
{
    if (s_tl) { wm_activate(s_tl); return; }
    window_t *w = wm_create("Tasks", 380, 250, WIN_DIALOG);
    w->icon = &icon16_tasks;
    ui_label(w, 10, 8, 360, 14, "&Running tasks:");
    s_tl_list = ui_listview(w, 10, 26, 360, 180, tl_get);
    static const char *const titles[] = {"Task", "Status", "Memory"};
    static const int widths[] = {190, 100, 66};
    ui_listview_columns(s_tl_list, 3, titles, widths);
    s_tl_list->on_action = tl_list_action;
    ui_button(w, 10, 216, 90, 24, "&Switch To", tl_switch);
    ui_button(w, 108, 216, 90, 24, "&End Task", tl_end);
    widget_t *c = ui_button(w, 292, 216, 78, 24, "Close", tl_close);
    w->cancel_btn = c;
    w->on_destroy = tl_destroy;
    s_tl = w;
    tl_refresh(NULL);
    s_tl_timer = timer_add(1000, true, tl_refresh, NULL);
    w->focus = s_tl_list;
    wm_show(w);
}

// ================================================================ Help

static window_t *s_help;
static widget_t *s_help_list;
static const char *const s_help_rows[][2] = {
    {"ANYWHERE", NULL},
    {"Ctrl+Esc or the GUI key", "Open the Bevel menu"},
    {"Alt+Tab / Alt+Shift+Tab", "Switch windows (hold Alt, release to pick)"},
    {"Alt+F4", "Close the window; on the desktop: shut down"},
    {"Alt+Space", "Window menu: Restore, Move, Size, Minimise, Maximise, Close"},
    {"Ctrl+Alt+Del", "Task list (close a task that hangs)"},
    {"GUI+D", "Show the desktop"},
    {"GUI+E", "Open Files"},
    {"F1", "This help"},
    {"MOVING AND SIZING", NULL},
    {"Alt+Space, M / S", "Start moving / sizing with the keyboard"},
    {"Arrow keys", "Move or resize (hold Ctrl for single pixels)"},
    {"Enter / Esc", "Keep / undo the change"},
    {"DESKTOP AND TASKBAR", NULL},
    {"Arrow keys, Home, End", "Select a desktop icon"},
    {"Enter", "Open the selected icon"},
    {"Letters", "Jump to the icon starting with that letter"},
    {"Tab", "Switch between desktop icons and the taskbar"},
    {"Left / Right, Enter", "On the taskbar: pick the menu button or a window"},
    {"Esc", "On the taskbar: back to the last window"},
    {"Menu key or Shift+F10", "Desktop menu"},
    {"WINDOWS AND DIALOGS", NULL},
    {"Tab / Shift+Tab", "Next / previous control"},
    {"Arrow keys", "Move inside lists, menus, radio groups, between buttons"},
    {"Enter", "Default button, or open the selection"},
    {"Space", "Press the focused button, tick a check box"},
    {"Esc", "Cancel or close the dialog"},
    {"Alt or F10", "Go to the menu bar"},
    {"Alt+underlined letter", "Open that menu or press that button"},
    {"Home, End, PgUp, PgDn", "Jump in lists and text"},
    {"MENUS", NULL},
    {"Arrow keys", "Move; Right opens a submenu, Left closes it"},
    {"Enter", "Choose the item"},
    {"Underlined letter", "Choose that item"},
    {"Esc", "Close one level"},
    {"FILES", NULL},
    {"Enter", "Open the file or folder"},
    {"Backspace", "Go to the parent folder"},
    {"Delete", "Delete (asks first)"},
    {"F2", "Rename"},
    {"Ctrl+C / Ctrl+X / Ctrl+V", "Copy / cut / paste files"},
    {"Ctrl+N / Ctrl+Shift+N", "New text file / new folder"},
    {"F5", "Refresh"},
    {"Letters", "Jump to the next matching name"},
    {"TEXT EDITOR", NULL},
    {"Ctrl+N / Ctrl+O / Ctrl+S", "New / open / save"},
    {"Ctrl+Shift+S", "Save as"},
    {"Shift+arrows", "Select text"},
    {"Ctrl+A", "Select all"},
    {"Ctrl+C / Ctrl+X / Ctrl+V", "Copy / cut / paste"},
    {"Ctrl+F / F3", "Find / find next"},
    {"Ctrl+Home / Ctrl+End", "Start / end of the file"},
    {"Ctrl+Left / Ctrl+Right", "Previous / next word"},
    {"PICTURE VIEWER", NULL},
    {"Arrow keys", "Scroll the picture"},
    {"+ / -", "Zoom in / out"},
    {"DOOM", NULL},
    {"Arrow keys", "Move and turn"},
    {"Ctrl / Space", "Fire / open doors and use switches"},
    {"Shift", "Run"},
    {", and .", "Step left / right"},
    {"1 to 7", "Choose a weapon"},
    {"Tab", "Map"},
    {"Esc, then arrows and Enter", "Game menu (new game, save, load, quit)"},
    {"GAMES AND APPS", NULL},
    {"Arrow keys, Space, Enter", "Play (see each game)"},
    {"Esc or Alt+F4", "Leave the game"},
};
#define HELP_N ((int)(sizeof(s_help_rows) / sizeof(s_help_rows[0])))

static void help_get(widget_t *lv, int i, lv_item_t *it)
{
    it->text = s_help_rows[i][0];
    it->col[0] = s_help_rows[i][1];
}
static void help_resize(window_t *w)
{
    ui_set_rect(s_help_list, 6, 6, wm_client_w(w) - 12, wm_client_h(w) - 12);
}
static void help_destroy(window_t *w) { s_help = NULL; }
static bool help_key(window_t *w, const event_t *e)
{
    if (e->key == K_ESC) { wm_close(w); return true; }
    return false;
}

void app_help(const char *arg)
{
    if (s_help) { wm_activate(s_help); return; }
    window_t *w = wm_create("Help: keyboard shortcuts", 480, 330, WIN_RESIZABLE);
    w->icon = &icon16_help;
    s_help_list = ui_listview(w, 6, 6, 468, 318, help_get);
    static const char *const titles[] = {"Key", "What it does"};
    static const int widths[] = {170, 280};
    ui_listview_columns(s_help_list, 2, titles, widths);
    ui_listview_set_count(s_help_list, HELP_N);
    w->on_resize = help_resize;
    w->on_destroy = help_destroy;
    w->on_key = help_key;
    s_help = w;
    wm_show(w);
}

// ================================================================ Benchmark

static window_t *s_bench;
static widget_t *s_bench_list;
static char s_bench_val[PERF_MAX][24];

static void bench_get(widget_t *lv, int i, lv_item_t *it)
{
    it->text = perf_results[i].name;
    const char *u = perf_results[i].unit;
    uint32_t v = perf_results[i].value;
    if (!strcmp(u, "KB/s")) snprintf(s_bench_val[i], 24, "%u.%u MB/s", (unsigned)(v / 1024), (unsigned)(v % 1024 * 10 / 1024));
    else if (strstr(u, "x10")) { char unit[12]; strlcpy(unit, u, sizeof(unit)); char *sp = strchr(unit, ' '); if (sp) *sp = 0; snprintf(s_bench_val[i], 24, "%u.%u %s", (unsigned)(v / 10), (unsigned)(v % 10), unit); }
    else snprintf(s_bench_val[i], 24, "%u %s", (unsigned)v, u);
    it->col[0] = s_bench_val[i];
}

static void bench_run(widget_t *b, int code)
{
    wm_busy(true);
    wm_pump(0);
    perf_run("all");
    wm_busy(false);
    ui_listview_set_count(s_bench_list, perf_count);
    comp_invalidate_all();
}
static void bench_resize(window_t *w)
{
    ui_set_rect(s_bench_list, 6, 40, wm_client_w(w) - 12, wm_client_h(w) - 46);
}
static void bench_destroy(window_t *w) { s_bench = NULL; }

void app_bench(const char *arg)
{
    if (s_bench) { wm_activate(s_bench); return; }
    window_t *w = wm_create("Benchmark", 400, 330, WIN_RESIZABLE);
    w->icon = &icon16_tasks;
    widget_t *run = ui_button(w, 6, 8, 100, 24, "&Run benchmark", bench_run);
    ui_label(w, 116, 14, 280, 14, "Takes about ten seconds. The display stays live.");
    s_bench_list = ui_listview(w, 6, 40, 388, 284, bench_get);
    static const char *const titles[] = {"Test", "Result"};
    static const int widths[] = {210, 150};
    ui_listview_columns(s_bench_list, 2, titles, widths);
    ui_listview_set_count(s_bench_list, perf_count);
    w->on_resize = bench_resize;
    w->on_destroy = bench_destroy;
    w->def_btn = run;
    s_bench = w;
    wm_show(w);
}

// ================================================================ Widget gallery

static const char *const s_fruit[] = {"Apple", "Apricot", "Banana", "Blackcurrant", "Cherry", "Cloudberry", "Damson",
    "Elderberry", "Fig", "Gooseberry", "Grape", "Kiwi", "Lemon", "Lime", "Mango", "Nectarine", "Orange", "Peach", "Pear", "Plum"};
static void fruit_get(widget_t *lv, int i, lv_item_t *it) { it->text = s_fruit[i]; it->icon = &icon16_file; }
static void gal_hello(widget_t *b, int code)
{
    widget_t *f = b->win->user;
    char msg[96];
    snprintf(msg, sizeof(msg), "Hello, %s!", ui_textfield_get(f));
    msgbox(b->win, "Greeting", msg, MB_INFO, MB_OK);
}
static void gal_close(widget_t *b, int code) { wm_close(b->win); }
enum { GC_ABOUT = 1, GC_CLOSE, GC_A, GC_B };
static const menu_item_t gal_file[] = {{"&About...", GC_ABOUT, NULL, NULL, 0}, {"-", 0, NULL, NULL, 0}, {"&Close\tAlt+F4", GC_CLOSE, NULL, NULL, 0}, {NULL, 0, NULL, NULL, 0}};
static const menu_item_t gal_more[] = {{"Option &A", GC_A, NULL, NULL, MI_CHECKED}, {"Option &B", GC_B, NULL, NULL, MI_DISABLED}, {NULL, 0, NULL, NULL, 0}};
static const menu_item_t gal_bar[] = {{"&File", 0, gal_file, NULL, 0}, {"&Options", 0, gal_more, NULL, 0}, {NULL, 0, NULL, NULL, 0}};
static void gal_cmd(window_t *w, int cmd)
{
    if (cmd == GC_ABOUT) msgbox(w, "About", "A window showing every control of the Bevel toolkit.\nAll of them work from the keyboard.", MB_INFO, MB_OK);
    else if (cmd == GC_CLOSE) wm_close(w);
}

void app_gallery(const char *arg)
{
    window_t *w = wm_create("Widget Gallery", 420, 260, 0);
    w->icon = &icon16_app;
    wm_set_menubar(w, gal_bar);
    w->on_command = gal_cmd;
    ui_label(w, 12, 12, 60, 14, "&Name:");
    widget_t *f = ui_textfield(w, 60, 9, 150, "World", 40);
    w->user = f;
    ui_button(w, 220, 7, 80, 24, "Say &hello", gal_hello);
    ui_groupbox(w, 12, 40, 190, 90, "Options");
    ui_checkbox(w, 24, 58, 160, "&Check me", true);
    ui_checkbox(w, 24, 78, 160, "&Disabled box", false)->flags |= WF_DISABLED;
    ui_radio(w, 24, 100, 70, "&One", 1, true);
    ui_radio(w, 84, 100, 60, "&Two", 1, false);
    ui_radio(w, 140, 100, 60, "Th&ree", 1, false);
    ui_label(w, 12, 140, 60, 14, "&Speed:");
    ui_slider(w, 60, 134, 140, 1, 10, 5);
    ui_label(w, 216, 40, 100, 14, "&Fruit:");
    widget_t *lv = ui_listview(w, 216, 56, 192, 160, fruit_get);
    ui_listview_set_count(lv, 20);
    widget_t *c = ui_button(w, 330, 226, 78, 24, "Close", gal_close);
    w->cancel_btn = c;
    wm_show(w);
}
