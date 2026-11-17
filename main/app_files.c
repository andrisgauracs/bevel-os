// Files: the file browser, file operations, the open/save dialog and "open by type".
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "fsutil.h"
#include "esp_heap_caps.h"

// ================================================================ filesystem helpers

void fs_join(char *out, int n, const char *dir, const char *name)
{
    snprintf(out, n, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

const char *fs_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    const char *slash = strrchr(name, '/');
    return (dot && (!slash || dot > slash)) ? dot + 1 : "";
}

const char *fs_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

bool fs_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
bool fs_is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }

static int ent_cmp(const void *a, const void *b)
{
    const fs_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

int fs_list(const char *dir, fs_entry_t **out)
{
    *out = NULL;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0, cap = 0;
    fs_entry_t *list = NULL;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        // the folder the SD card is mounted on only shows while a card is in the slot
        if (!storage_sd_ok() && !strcmp(dir, DISK_ROOT) && !strcmp(de->d_name, "SD")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            fs_entry_t *nl = os_grow(list, n * sizeof(fs_entry_t), cap * sizeof(fs_entry_t));
            if (!nl) break;
            list = nl;
        }
        fs_entry_t *e = &list[n++];
        strlcpy(e->name, de->d_name, sizeof(e->name));
        e->is_dir = de->d_type == DT_DIR;
        e->size = 0;
        if (!e->is_dir) {
            char p[300];
            struct stat st;
            fs_join(p, sizeof(p), dir, e->name);
            if (stat(p, &st) == 0) e->size = st.st_size;
        }
    }
    closedir(d);
    if (n) qsort(list, n, sizeof(fs_entry_t), ent_cmp);
    *out = list;
    return n;
}

bool fs_delete(const char *path)
{
    if (fs_is_dir(path)) {
        fs_entry_t *list;
        int n = fs_list(path, &list);
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            char p[300];
            fs_join(p, sizeof(p), path, list[i].name);
            ok = fs_delete(p);
        }
        free(list);
        return ok && rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

bool fs_copy(const char *src, const char *dst)
{
    if (fs_is_dir(src)) {
        if (mkdir(dst, 0777) != 0 && !fs_is_dir(dst)) return false;
        fs_entry_t *list;
        int n = fs_list(src, &list);
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            char a[300], b[300];
            fs_join(a, sizeof(a), src, list[i].name);
            fs_join(b, sizeof(b), dst, list[i].name);
            ok = fs_copy(a, b);
        }
        free(list);
        return ok;
    }
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    bool ok = buf != NULL;
    size_t k;
    while (ok && (k = fread(buf, 1, 16384, in)) > 0) ok = fwrite(buf, 1, k, out) == k;
    free(buf);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) unlink(dst);
    return ok;
}

bool fs_valid_name(const char *name)
{
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > 60) return false;
    return strpbrk(name, "/\\:*?\"<>|") == NULL;
}

// Read a whole file into PSRAM (NUL-terminated). Caller frees.
char *fs_read_all(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (buf) { n = fread(buf, 1, n, f); buf[n] = 0; if (len) *len = n; }
    fclose(f);
    return buf;
}

// ================================================================ open by type

static bool ext_in(const char *ext, const char *list)
{
    char e[10];
    int n = 0;
    for (; ext[n] && n < 8; n++) e[n] = tolower((unsigned char)ext[n]);
    e[n] = 0;
    if (!n) return false;
    const char *p = list;
    while (*p) {
        const char *q = strchr(p, ' ');
        int l = q ? (int)(q - p) : (int)strlen(p);
        if (l == n && !strncmp(p, e, n)) return true;
        if (!q) break;
        p = q + 1;
    }
    return false;
}

const char *fs_type_name(const fs_entry_t *e)
{
    if (e->is_dir) return "Folder";
    const char *x = fs_ext(e->name);
    if (ext_in(x, "txt md log ini cfg csv")) return "Text";
    if (ext_in(x, "c h py sh json")) return "Source text";
    if (ext_in(x, "bmp")) return "Picture";
    if (ext_in(x, "wav")) return "Sound";
    if (ext_in(x, "elf app")) return "Program";
    return "File";
}

const bitmap_t *fs_type_icon(const fs_entry_t *e)
{
    if (e->is_dir) return &icon16_folder;
    const char *x = fs_ext(e->name);
    if (ext_in(x, "txt md log ini cfg csv c h py sh json")) return &icon16_text;
    if (ext_in(x, "bmp")) return &icon16_image;
    if (ext_in(x, "elf app")) return strcasestr(e->name, "game") || strcasestr(e->name, "snake") || strcasestr(e->name, "blocks") ? &icon16_game : &icon16_app;
    return &icon16_file;
}

void os_open_file(window_t *owner, const char *path)
{
    const char *x = fs_ext(path);
    if (ext_in(x, "bmp")) app_viewer(path);
#if BOARD_HAS_AUDIO
    else if (ext_in(x, "wav")) app_player(path);
#endif
    else if (ext_in(x, "elf app")) os_run_elf(path);
    else if (ext_in(x, "txt md log ini cfg csv c h py sh json")) app_editor(path);
    else if (msgbox(owner, "Open", "Bevel OS does not know this kind of file.\nOpen it in the text editor?", MB_ASK, MB_YESNO) == ID_YES) app_editor(path);
}

// ================================================================ file browser

typedef struct {
    window_t *win;
    widget_t *list, *path_lbl, *status;
    char dir[256];
    fs_entry_t *ents;
    int n;
} files_t;

static char s_fclip[300];
static bool s_fclip_cut;

enum { FC_OPEN = 1, FC_NEW_FOLDER, FC_NEW_FILE, FC_RENAME, FC_DELETE, FC_CLOSE, FC_CUT, FC_COPY, FC_PASTE, FC_REFRESH, FC_UP, FC_NEW_WINDOW };

static void files_get(widget_t *lv, int i, lv_item_t *it)
{
    files_t *f = lv->win->user;
    static char size[24];
    fs_entry_t *e = &f->ents[i];
    bool card = e->is_dir && !strcmp(f->dir, DISK_ROOT) && !strcmp(e->name, "SD");
    it->text = e->name;
    it->icon = card ? &icon16_drive : fs_type_icon(e);
    if (e->is_dir) size[0] = 0; else os_fmt_size(size, sizeof(size), e->size);
    it->col[0] = size;
    it->col[1] = card ? "SD card" : fs_type_name(e);
}

static void files_load(files_t *f, const char *select)
{
    free(f->ents);
    f->ents = NULL;
    f->n = fs_list(f->dir, &f->ents);
    if (f->n < 0) {
        f->n = 0;
        if (strcmp(f->dir, DISK_ROOT)) { strcpy(f->dir, DISK_ROOT); f->n = fs_list(f->dir, &f->ents); if (f->n < 0) f->n = 0; }
    }
    ui_listview_set_count(f->list, f->n);
    int sel = f->n ? 0 : -1;
    if (select) for (int i = 0; i < f->n; i++) if (!strcasecmp(f->ents[i].name, select)) sel = i;
    ui_listview_select(f->list, sel);
    char title[300];
    snprintf(title, sizeof(title), "%s", strlen(f->dir) > strlen(DISK_ROOT) ? f->dir + strlen(DISK_ROOT) : "/");
    ui_set_text(f->path_lbl, title);
    snprintf(title, sizeof(title), "Files - %s", strcmp(f->dir, DISK_ROOT) ? fs_basename(f->dir) : "Drive");
    wm_set_title(f->win, title);
    uint64_t total = 0, fr = 0;
    char a[20] = "?", b[20] = "?", st[96];
    if (storage_df_path(f->dir, &total, &fr)) { os_fmt_size(a, sizeof(a), fr); os_fmt_size(b, sizeof(b), total); }
    snprintf(st, sizeof(st), "%d item%s      %s free of %s", f->n, f->n == 1 ? "" : "s", a, b);
    ui_set_text(f->status, st);
}

static fs_entry_t *files_sel(files_t *f)
{
    int i = ui_listview_sel(f->list);
    return (i >= 0 && i < f->n) ? &f->ents[i] : NULL;
}

static void files_up(files_t *f)
{
    if (!strcmp(f->dir, DISK_ROOT)) return;
    char was[68];
    strlcpy(was, fs_basename(f->dir), sizeof(was));
    *strrchr(f->dir, '/') = 0;
    files_load(f, was);
}

static void files_open(files_t *f)
{
    fs_entry_t *e = files_sel(f);
    if (!e) return;
    char p[300];
    fs_join(p, sizeof(p), f->dir, e->name);
    if (e->is_dir) { strlcpy(f->dir, p, sizeof(f->dir)); files_load(f, NULL); }
    else os_open_file(f->win, p);
}

static void files_new(files_t *f, bool folder)
{
    char name[64];
    strcpy(name, folder ? "New folder" : "New file.txt");
    for (;;) {
        if (!inputbox(f->win, folder ? "New Folder" : "New Text File", folder ? "Folder name:" : "File name:", name, sizeof(name))) return;
        if (!fs_valid_name(name)) { msgbox(f->win, "Files", "That name is not allowed.\nNames cannot contain / \\ : * ? \" < > |", MB_WARN, MB_OK); continue; }
        char p[300];
        fs_join(p, sizeof(p), f->dir, name);
        if (fs_exists(p)) { msgbox(f->win, "Files", "Something with that name already exists here.", MB_WARN, MB_OK); continue; }
        bool ok;
        if (folder) ok = mkdir(p, 0777) == 0;
        else { FILE *fp = fopen(p, "wb"); ok = fp != NULL; if (fp) fclose(fp); }
        if (!ok) msgbox(f->win, "Files", "Could not create it.", MB_ERROR, MB_OK);
        files_load(f, name);
        return;
    }
}

static void files_rename(files_t *f)
{
    fs_entry_t *e = files_sel(f);
    if (!e) return;
    char name[64];
    strlcpy(name, e->name, sizeof(name));
    for (;;) {
        if (!inputbox(f->win, "Rename", "New name:", name, sizeof(name))) return;
        if (!strcmp(name, e->name)) return;
        if (!fs_valid_name(name)) { msgbox(f->win, "Rename", "That name is not allowed.", MB_WARN, MB_OK); continue; }
        char a[300], b[300];
        fs_join(a, sizeof(a), f->dir, e->name);
        fs_join(b, sizeof(b), f->dir, name);
        if (fs_exists(b) && strcasecmp(name, e->name)) { msgbox(f->win, "Rename", "Something with that name already exists here.", MB_WARN, MB_OK); continue; }
        if (rename(a, b) != 0) msgbox(f->win, "Rename", "Could not rename it.", MB_ERROR, MB_OK);
        files_load(f, name);
        return;
    }
}

static void files_delete(files_t *f)
{
    fs_entry_t *e = files_sel(f);
    if (!e) return;
    char msg[160], p[300];
    snprintf(msg, sizeof(msg), e->is_dir ? "Delete the folder \"%s\" and everything in it?" : "Delete \"%s\"?", e->name);
    if (msgbox(f->win, "Delete", msg, MB_WARN, MB_YESNO) != ID_YES) return;
    fs_join(p, sizeof(p), f->dir, e->name);
    int idx = ui_listview_sel(f->list);
    wm_busy(true);
    bool ok = fs_delete(p);
    wm_busy(false);
    if (!ok) msgbox(f->win, "Delete", "Could not delete it.", MB_ERROR, MB_OK);
    files_load(f, NULL);
    ui_listview_select(f->list, idx < f->n ? idx : f->n - 1);
}

static void files_clip(files_t *f, bool cut)
{
    fs_entry_t *e = files_sel(f);
    if (!e) return;
    fs_join(s_fclip, sizeof(s_fclip), f->dir, e->name);
    s_fclip_cut = cut;
}

static void files_paste(files_t *f)
{
    if (!s_fclip[0]) return;
    if (!fs_exists(s_fclip)) { msgbox(f->win, "Paste", "The item to paste is no longer there.", MB_WARN, MB_OK); s_fclip[0] = 0; return; }
    int sl = strlen(s_fclip);
    if (fs_is_dir(s_fclip) && !strncmp(f->dir, s_fclip, sl) && (f->dir[sl] == '/' || f->dir[sl] == 0)) {
        msgbox(f->win, "Paste", "A folder cannot be put inside itself.", MB_WARN, MB_OK);
        return;
    }
    char name[80], dst[300];
    strlcpy(name, fs_basename(s_fclip), sizeof(name));
    fs_join(dst, sizeof(dst), f->dir, name);
    if (!strcmp(dst, s_fclip)) {
        if (s_fclip_cut) return;
        // copy next to itself: "Copy of name"
        snprintf(name, sizeof(name), "Copy of %s", fs_basename(s_fclip));
        name[60] = 0;
        fs_join(dst, sizeof(dst), f->dir, name);
    }
    if (fs_exists(dst)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "\"%s\" already exists here. Replace it?", name);
        if (msgbox(f->win, "Paste", msg, MB_ASK, MB_YESNO) != ID_YES) return;
        fs_delete(dst);
    }
    wm_busy(true);
    bool ok;
    if (s_fclip_cut) {
        ok = rename(s_fclip, dst) == 0;
        if (!ok) ok = fs_copy(s_fclip, dst) && fs_delete(s_fclip);   // between the drive and the SD card
        if (ok) s_fclip[0] = 0;
    }
    else ok = fs_copy(s_fclip, dst);
    wm_busy(false);
    if (!ok) msgbox(f->win, "Paste", "Could not finish. The drive may be full.", MB_ERROR, MB_OK);
    files_load(f, name);
}

static void files_command(window_t *w, int cmd);

// The SD card came or went: every open Files window lists its folder again.
void files_refresh_all(void)
{
    for (window_t *w = wm_top(); w; w = w->prev) {
        if (w->on_command != files_command || (w->flags & WIN_DEAD)) continue;
        files_t *f = w->user;
        fs_entry_t *e = files_sel(f);
        char keep[68] = "";
        if (e) strlcpy(keep, e->name, sizeof(keep));
        if (!fs_is_dir(f->dir)) strcpy(f->dir, DISK_ROOT);
        files_load(f, keep);
    }
}

static void files_command(window_t *w, int cmd)
{
    files_t *f = w->user;
    switch (cmd) {
    case FC_OPEN: files_open(f); break;
    case FC_NEW_FOLDER: files_new(f, true); break;
    case FC_NEW_FILE: files_new(f, false); break;
    case FC_RENAME: files_rename(f); break;
    case FC_DELETE: files_delete(f); break;
    case FC_CLOSE: wm_close(w); break;
    case FC_CUT: files_clip(f, true); break;
    case FC_COPY: files_clip(f, false); break;
    case FC_PASTE: files_paste(f); break;
    case FC_REFRESH: { fs_entry_t *e = files_sel(f); char n[68] = ""; if (e) strlcpy(n, e->name, sizeof(n)); files_load(f, n); break; }
    case FC_UP: files_up(f); break;
    case FC_NEW_WINDOW: app_files(f->dir); break;
    }
}

static int files_menu_state(window_t *w, int cmd)
{
    files_t *f = w->user;
    bool has = files_sel(f) != NULL;
    switch (cmd) {
    case FC_OPEN: case FC_RENAME: case FC_DELETE: case FC_CUT: case FC_COPY: return has ? 0 : MI_DISABLED;
    case FC_PASTE: return s_fclip[0] ? 0 : MI_DISABLED;
    case FC_UP: return strcmp(f->dir, DISK_ROOT) ? 0 : MI_DISABLED;
    }
    return 0;
}

static const menu_item_t fm_file[] = {
    {"&Open\tEnter", FC_OPEN, NULL, NULL, 0},
    {"-", 0, NULL, NULL, 0},
    {"New &Folder...\tCtrl+Shift+N", FC_NEW_FOLDER, NULL, &icon16_folder, 0},
    {"&New Text File...\tCtrl+N", FC_NEW_FILE, NULL, &icon16_text, 0},
    {"-", 0, NULL, NULL, 0},
    {"&Rename...\tF2", FC_RENAME, NULL, NULL, 0},
    {"&Delete\tDel", FC_DELETE, NULL, NULL, 0},
    {"-", 0, NULL, NULL, 0},
    {"New &Window", FC_NEW_WINDOW, NULL, NULL, 0},
    {"&Close\tAlt+F4", FC_CLOSE, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t fm_edit[] = {
    {"Cu&t\tCtrl+X", FC_CUT, NULL, NULL, 0},
    {"&Copy\tCtrl+C", FC_COPY, NULL, NULL, 0},
    {"&Paste\tCtrl+V", FC_PASTE, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t fm_go[] = {
    {"&Up one folder\tBackspace", FC_UP, NULL, &icon16_up, 0},
    {"&Refresh\tF5", FC_REFRESH, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t fm_bar[] = {{"&File", 0, fm_file, NULL, 0}, {"&Edit", 0, fm_edit, NULL, 0}, {"&Go", 0, fm_go, NULL, 0}, {NULL, 0, NULL, NULL, 0}};

static menu_item_t s_ctx_items[10];
static void files_ctx_cb(void *ctx, int cmd)
{
    window_t *w = ctx;
    if (cmd > 0 && wm_valid(w)) files_command(w, cmd);
}

static void files_list_action(widget_t *lv, int code)
{
    files_t *f = lv->win->user;
    if (code == UI_ACTIVATE) files_open(f);
    else if (code == UI_CONTEXT) {
        bool has = files_sel(f) != NULL;
        int n = 0;
        if (has) {
            s_ctx_items[n++] = (menu_item_t){"&Open", FC_OPEN, NULL, NULL, 0};
            s_ctx_items[n++] = (menu_item_t){"-", 0, NULL, NULL, 0};
            s_ctx_items[n++] = (menu_item_t){"Cu&t", FC_CUT, NULL, NULL, 0};
            s_ctx_items[n++] = (menu_item_t){"&Copy", FC_COPY, NULL, NULL, 0};
        }
        s_ctx_items[n++] = (menu_item_t){"&Paste", FC_PASTE, NULL, NULL, s_fclip[0] ? 0 : MI_DISABLED};
        if (has) {
            s_ctx_items[n++] = (menu_item_t){"&Rename...", FC_RENAME, NULL, NULL, 0};
            s_ctx_items[n++] = (menu_item_t){"&Delete", FC_DELETE, NULL, NULL, 0};
        }
        s_ctx_items[n++] = (menu_item_t){"-", 0, NULL, NULL, 0};
        s_ctx_items[n++] = (menu_item_t){"New &Folder...", FC_NEW_FOLDER, NULL, NULL, 0};
        s_ctx_items[n] = (menu_item_t){NULL, 0, NULL, NULL, 0};
        int x, y;
        ui_listview_context_pos(lv, &x, &y);
        menu_popup(s_ctx_items, x, y, false, files_ctx_cb, lv->win);
    }
}

static bool files_key(window_t *w, const event_t *e)
{
    files_t *f = w->user;
    if (w->focus != f->list) return false;
    bool ctrl = e->mods & MOD_CTRL, shift = e->mods & MOD_SHIFT;
    if (e->mods & MOD_ALT) return false;
    if (ctrl) {
        switch (e->key) {
        case K_C: files_clip(f, false); return true;
        case K_X: files_clip(f, true); return true;
        case K_V: files_paste(f); return true;
        case K_N: files_new(f, shift); return true;
        }
        return false;
    }
    switch (e->key) {
    case K_BKSP: files_up(f); return true;
    case K_DEL: files_delete(f); return true;
    case K_F2: files_rename(f); return true;
    case K_F5: files_command(w, FC_REFRESH); return true;
    }
    return false;
}

static void files_resize(window_t *w)
{
    files_t *f = w->user;
    int cw = wm_client_w(w), ch = wm_client_h(w);
    ui_set_rect(f->path_lbl, 30, 6, cw - 36, 14);
    ui_set_rect(f->list, 4, 26, cw - 8, ch - 26 - 22);
    ui_set_rect(f->status, 8, ch - 17, cw - 16, 14);
}

static void files_destroy(window_t *w)
{
    files_t *f = w->user;
    free(f->ents);
    free(f);
}

void app_files(const char *arg)
{
    if (!storage_ok()) { msgbox(NULL, "Files", "The drive could not be mounted.", MB_ERROR, MB_OK); return; }
    files_t *f = calloc(1, sizeof(files_t));
    window_t *w = wm_create("Files", 440, 300, WIN_RESIZABLE);
    w->icon = &icon16_drive;
    w->user = f;
    f->win = w;
    wm_set_menubar(w, fm_bar);
    strlcpy(f->dir, (arg && fs_is_dir(arg)) ? arg : DISK_ROOT, sizeof(f->dir));
    ui_icon(w, 8, 4, &icon16_folder);
    f->path_lbl = ui_label(w, 30, 6, 400, 14, "/");
    f->list = ui_listview(w, 4, 26, 432, 250, files_get);
    static const char *const titles[] = {"Name", "Size", "Type"};
    static const int widths[] = {230, 90, 90};
    ui_listview_columns(f->list, 3, titles, widths);
    f->list->on_action = files_list_action;
    f->status = ui_label(w, 8, 282, 420, 14, "");
    w->on_command = files_command;
    w->on_menu_state = files_menu_state;
    w->on_key = files_key;
    w->on_resize = files_resize;
    w->on_destroy = files_destroy;
    w->min_w = 260; w->min_h = 160;
    files_load(f, NULL);
    wm_show(w);
}

// ================================================================ open / save dialog

typedef struct {
    window_t *win;
    widget_t *list, *field, *where;
    char dir[256];
    fs_entry_t *ents;
    int n;
    bool save;
    char *out;
    int out_size;
} fdlg_t;

static void fdlg_get(widget_t *lv, int i, lv_item_t *it)
{
    fdlg_t *d = lv->win->user;
    static char size[24];
    it->text = d->ents[i].name;
    it->icon = fs_type_icon(&d->ents[i]);
    if (d->ents[i].is_dir) size[0] = 0; else os_fmt_size(size, sizeof(size), d->ents[i].size);
    it->col[0] = size;
}

static void fdlg_load(fdlg_t *d)
{
    free(d->ents);
    d->n = fs_list(d->dir, &d->ents);
    if (d->n < 0) d->n = 0;
    ui_listview_set_count(d->list, d->n);
    ui_listview_select(d->list, d->n ? 0 : -1);
    ui_set_text(d->where, strlen(d->dir) > strlen(DISK_ROOT) ? d->dir + strlen(DISK_ROOT) : "/");
}

static void fdlg_accept(fdlg_t *d)
{
    char name[80], p[300];
    strlcpy(name, ui_textfield_get(d->field), sizeof(name));
    if (!name[0]) return;
    if (!strcmp(name, "..")) {
        if (strcmp(d->dir, DISK_ROOT)) { *strrchr(d->dir, '/') = 0; fdlg_load(d); }
        ui_set_text(d->field, ""); return;
    }
    fs_join(p, sizeof(p), d->dir, name);
    if (fs_is_dir(p)) { strlcpy(d->dir, p, sizeof(d->dir)); fdlg_load(d); ui_focus(d->list); return; }
    if (d->save) {
        if (!fs_valid_name(name)) { msgbox(d->win, "Save", "That name is not allowed.", MB_WARN, MB_OK); return; }
        if (!fs_ext(name)[0] && strlen(p) < sizeof(p) - 5) strcat(p, ".txt");
        if (fs_exists(p)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "\"%s\" already exists. Replace it?", fs_basename(p));
            if (msgbox(d->win, "Save", msg, MB_ASK, MB_YESNO) != ID_YES) return;
        }
    } else if (!fs_exists(p)) {
        msgbox(d->win, "Open", "There is no file with that name here.", MB_WARN, MB_OK);
        return;
    }
    strlcpy(d->out, p, d->out_size);
    wm_end_modal(d->win, ID_OK);
}

static void fdlg_list_action(widget_t *lv, int code)
{
    fdlg_t *d = lv->win->user;
    int i = ui_listview_sel(lv);
    if (i < 0 || i >= d->n) return;
    if (code == UI_SELECT && !d->ents[i].is_dir) {
        // show the highlighted file in the name box without stealing focus
        ui_textfield_set(d->field, d->ents[i].name);
    } else if (code == UI_ACTIVATE) {
        if (d->ents[i].is_dir) {
            char p[300];
            fs_join(p, sizeof(p), d->dir, d->ents[i].name);
            strlcpy(d->dir, p, sizeof(d->dir));
            fdlg_load(d);
        } else {
            char p[300];
            fs_join(p, sizeof(p), d->dir, d->ents[i].name);
            if (d->save) {
                char msg[160];
                snprintf(msg, sizeof(msg), "\"%s\" already exists. Replace it?", d->ents[i].name);
                if (msgbox(d->win, "Save", msg, MB_ASK, MB_YESNO) != ID_YES) return;
            }
            strlcpy(d->out, p, d->out_size);
            wm_end_modal(d->win, ID_OK);
        }
    }
}

static void fdlg_ok(widget_t *b, int code) { fdlg_accept(b->win->user); }
static void fdlg_cancel(widget_t *b, int code) { wm_end_modal(b->win, ID_CANCEL); }

static bool fdlg_key(window_t *w, const event_t *e)
{
    fdlg_t *d = w->user;
    if (e->key == K_BKSP && w->focus == d->list && !(e->mods & (MOD_CTRL | MOD_ALT))) {
        if (strcmp(d->dir, DISK_ROOT)) { *strrchr(d->dir, '/') = 0; fdlg_load(d); }
        return true;
    }
    return false;
}

bool file_dialog(window_t *owner, bool save, const char *title, char *path, int path_size)
{
    fdlg_t d = {0};
    window_t *w = wm_create(title, 400, 284, WIN_DIALOG);
    w->owner = owner;
    w->user = &d;
    d.win = w; d.save = save; d.out = path; d.out_size = path_size;
    const char *name = "";
    if (path[0]) {
        strlcpy(d.dir, path, sizeof(d.dir));
        char *s = strrchr(d.dir, '/');
        if (s && s > d.dir) { *s = 0; name = fs_basename(path); }
    }
    if (!d.dir[0] || !fs_is_dir(d.dir)) strcpy(d.dir, DISK_ROOT "/Documents");
    if (!fs_is_dir(d.dir)) strcpy(d.dir, DISK_ROOT);
    ui_label(w, 10, 10, 60, 14, save ? "Save in:" : "Look in:");
    d.where = ui_label(w, 76, 10, 310, 14, "/");
    d.list = ui_listview(w, 10, 28, 380, 186, fdlg_get);
    static const char *const titles[] = {"Name", "Size"};
    static const int widths[] = {270, 90};
    ui_listview_columns(d.list, 2, titles, widths);
    d.list->on_action = fdlg_list_action;
    ui_label(w, 10, 225, 64, 14, "File &name:");
    d.field = ui_textfield(w, 76, 222, 314, name, 60);
    widget_t *ok = ui_button(w, 236, 250, 74, 24, save ? "Save" : "Open", fdlg_ok);
    widget_t *ca = ui_button(w, 316, 250, 74, 24, "Cancel", fdlg_cancel);
    w->def_btn = ok; w->cancel_btn = ca;
    w->on_key = fdlg_key;
    fdlg_load(&d);
    w->focus = save ? d.field : d.list;
    bool ok_res = wm_run_modal(w) == ID_OK;
    free(d.ents);
    return ok_res;
}
