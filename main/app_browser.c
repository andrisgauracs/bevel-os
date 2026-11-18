// Browser: a small early-90s style web browser. HTTP and HTTPS, HTML as text, headings, lists,
// links, preformatted text, rules and simple search forms. No scripts, no style sheets.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "comp.h"
#include "net.h"
#include "fsutil.h"

#define MAX_PAGE (2 * 1024 * 1024)
#define URL_MAX 512
#define HIST_MAX 24
#define MARGIN 8

// box styles
#define ST_BOLD 1
#define ST_MONO 2
#define ST_BIG  4
#define ST_LINK 8
#define ST_DIM  16
#define ST_HR   32
#define ST_FIELD 64

typedef struct { int32_t y; int16_t x, w; uint8_t h, style; int16_t link; uint32_t off; uint16_t len; } box_t;

typedef struct {
    window_t *win;
    widget_t *b_back, *b_fwd, *b_reload, *b_home, *url_f, *view, *status;
    char url[URL_MAX];
    char title[96];
    char *src; int src_len; bool plain;
    char *text; int text_len, text_cap;
    box_t *boxes; int nbox, box_cap;
    char **links; int nlinks, link_cap;
    int doc_h, scroll, focus_link, laid_w;
    scroll_t sb; bool sb_drag;
    char *hist[HIST_MAX]; int hpos, hlen;
    // loading
    volatile bool cancel; volatile int progress; bool loading; int gen, timer;
} browser_t;

static browser_t *s_br;
static void br_go(browser_t *b, const char *url, bool push);

// ================================================================ text pool, boxes, links

static int pool_add(browser_t *b, const char *s, int n)
{
    if (b->text_len + n + 1 > b->text_cap) {
        int nc = b->text_cap ? b->text_cap * 2 : 16384;
        while (nc < b->text_len + n + 1) nc *= 2;
        char *nt = os_grow(b->text, b->text_len, nc);
        if (!nt) return -1;
        b->text = nt; b->text_cap = nc;
    }
    int off = b->text_len;
    memcpy(b->text + off, s, n);
    b->text_len += n;
    return off;
}

static box_t *box_add(browser_t *b)
{
    if (b->nbox == b->box_cap) {
        int nc = b->box_cap ? b->box_cap * 2 : 1024;
        if (nc > 60000) return NULL;
        box_t *nb = os_grow(b->boxes, b->nbox * sizeof(box_t), nc * sizeof(box_t));
        if (!nb) return NULL;
        b->boxes = nb; b->box_cap = nc;
    }
    box_t *x = &b->boxes[b->nbox++];
    memset(x, 0, sizeof(*x));
    x->link = -1;
    return x;
}

static int link_add(browser_t *b, const char *url)
{
    if (b->nlinks >= 4000) return -1;
    if (b->nlinks == b->link_cap) {
        int nc = b->link_cap ? b->link_cap * 2 : 128;
        char **nl = os_grow(b->links, b->nlinks * sizeof(char *), nc * sizeof(char *));
        if (!nl) return -1;
        b->links = nl; b->link_cap = nc;
    }
    char *c = heap_caps_malloc(strlen(url) + 1, MALLOC_CAP_SPIRAM);
    if (!c) return -1;
    strcpy(c, url);
    b->links[b->nlinks] = c;
    return b->nlinks++;
}

static void page_free_layout(browser_t *b)
{
    for (int i = 0; i < b->nlinks; i++) free(b->links[i]);
    b->nlinks = 0; b->nbox = 0; b->text_len = 0; b->doc_h = 0;
}

// ================================================================ URLs

static void url_resolve(const char *base, const char *href, char *out, int n)
{
    while (*href == ' ') href++;
    if (!strncasecmp(href, "http://", 7) || !strncasecmp(href, "https://", 8) || !strncasecmp(href, "file:", 5) ||
        !strncasecmp(href, "about:", 6)) { strlcpy(out, href, n); goto strip; }
    if (strchr(href, ':') && strchr(href, ':') < href + 12 && !strchr(href, '/')) { out[0] = 0; return; } // mailto:, javascript:, tel:
    {
        const char *p = strstr(base, "://");
        const char *host = p ? p + 3 : base;
        const char *path = strchr(host, '/');
        int scheme_len = p ? (int)(p - base) + 1 : 0;
        int origin_len = path ? (int)(path - base) : (int)strlen(base);
        if (href[0] == '/' && href[1] == '/') snprintf(out, n, "%.*s%s", scheme_len, base, href);
        else if (href[0] == '/') snprintf(out, n, "%.*s%s", origin_len, base, href);
        else if (href[0] == '#') { out[0] = 0; return; }
        else {
            // relative to the directory of the base (ignoring its query)
            char dir[URL_MAX];
            strlcpy(dir, base, sizeof(dir));
            char *q = strpbrk(dir + origin_len, "?#");
            if (q) *q = 0;
            if (href[0] == '?') snprintf(out, n, "%s%s", dir, href);
            else {
                char *slash = strrchr(dir + origin_len, '/');
                if (slash) slash[1] = 0; else strlcat(dir, "/", sizeof(dir));
                snprintf(out, n, "%s%s", dir, href);
            }
        }
    }
strip:
    { char *h = strchr(out, '#'); if (h) *h = 0; }
    // collapse /./ and /dir/../ in the path
    char *p = strstr(out, "://");
    char *path = p ? strchr(p + 3, '/') : NULL;
    if (path) {
        char *q = strchr(path, '?');
        char tail[URL_MAX] = "";
        if (q) { strlcpy(tail, q, sizeof(tail)); *q = 0; }
        char *r;
        while ((r = strstr(path, "/./")) != NULL) memmove(r, r + 2, strlen(r + 2) + 1);
        while ((r = strstr(path, "/../")) != NULL) {
            char *prev = r;
            while (prev > path && *--prev != '/') { }
            memmove(prev, r + 3, strlen(r + 3) + 1);
        }
        strlcat(out, tail, n);
    }
    // spaces are not allowed in a request line
    for (char *c = out; *c; c++) if (*c == ' ') *c = '+';
}

static void url_encode(const char *in, char *out, int n)
{
    int o = 0;
    for (; *in && o < n - 4; in++) {
        unsigned char c = *in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else o += snprintf(out + o, 4, "%%%02X", c);
    }
    out[o] = 0;
}

// ================================================================ HTML to boxes

typedef struct {
    browser_t *b;
    int width, x, y, line_h, line_first;
    int indent;
    uint8_t style;
    int link;
    bool pre, space, in_title, gap_done;
    int bold, mono, big, dim;
    int list_depth, list_num[8];
    char word[200]; int wlen;
    char form_action[URL_MAX], form_hidden[256];
    bool skip_form;
} lay_t;

static int style_font(uint8_t st) { return (st & ST_MONO) ? FONT_MONO : (st & (ST_BOLD | ST_BIG)) ? FONT_UI_BOLD : FONT_UI; }
static int style_h(uint8_t st) { return (st & ST_BIG) ? 28 : (st & ST_MONO) ? 16 : 15; }
static int text_w(const char *s, int n, uint8_t st)
{
    int w = gfx_text_w(s, n, style_font(st));
    return (st & ST_BIG) ? w * 2 : w;
}

static void lay_newline(lay_t *L, bool force)
{
    if (L->x == L->indent && !force) return;
    int h = L->line_h ? L->line_h : 15;
    // boxes shorter than the line sit on its bottom edge
    for (int i = L->line_first; i < L->b->nbox; i++) L->b->boxes[i].y = L->y + (h - L->b->boxes[i].h);
    L->y += h;
    L->x = L->indent;
    L->line_h = 0;
    L->line_first = L->b->nbox;
    L->space = false;
}

static void lay_block(lay_t *L, int gap)
{
    lay_newline(L, false);
    if (gap && !L->gap_done && L->y > MARGIN) { L->y += gap; L->gap_done = true; }
}

static void lay_box(lay_t *L, const char *s, int n, uint8_t st, int forced_w)
{
    int w = forced_w ? forced_w : text_w(s, n, st);
    int sp = L->space && L->x > L->indent ? text_w(" ", 1, st) : 0;
    if (L->x + sp + w > L->width && L->x > L->indent && !L->pre) { lay_newline(L, false); sp = 0; }
    box_t *x = box_add(L->b);
    if (!x) return;
    int off = n ? pool_add(L->b, s, n) : 0;
    if (off < 0) { L->b->nbox--; return; }
    x->x = L->x + sp; x->w = w; x->h = style_h(st); x->style = st; x->link = L->link;
    x->off = off; x->len = n;
    if (x->h > L->line_h) L->line_h = x->h;
    L->x += sp + w;
    L->space = false;
    L->gap_done = false;
}

static uint8_t cur_style(lay_t *L)
{
    return (L->bold ? ST_BOLD : 0) | (L->mono || L->pre ? ST_MONO : 0) | (L->big ? ST_BIG : 0) | (L->link >= 0 ? ST_LINK : 0) |
           (L->dim ? ST_DIM : 0);
}

static void lay_flush_word(lay_t *L)
{
    if (!L->wlen) return;
    if (L->in_title) {
        int tl = strlen(L->b->title);
        if (tl && tl < (int)sizeof(L->b->title) - 2) L->b->title[tl++] = ' ';
        for (int i = 0; i < L->wlen && tl < (int)sizeof(L->b->title) - 1; i++) L->b->title[tl++] = L->word[i];
        L->b->title[tl] = 0;
    } else {
        lay_box(L, L->word, L->wlen, cur_style(L), 0);
    }
    L->wlen = 0;
}

static void lay_char(lay_t *L, char c)
{
    if (L->pre) {
        if (c == '\n') { lay_flush_word(L); lay_newline(L, true); return; }
        if (c == '\t') c = ' ';
        if (L->wlen == sizeof(L->word)) lay_flush_word(L);
        L->word[L->wlen++] = c;
        return;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { lay_flush_word(L); L->space = true; return; }
    if (L->wlen == sizeof(L->word)) lay_flush_word(L);   // very long "word": break it
    L->word[L->wlen++] = c;
}

// One Unicode code point to what our ASCII fonts can show.
static void lay_codepoint(lay_t *L, unsigned cp)
{
    if (cp < 0x80) { lay_char(L, cp < 32 && cp != '\n' && cp != '\t' ? ' ' : (char)cp); return; }
    static const char *lat = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp == 0xA0) { lay_char(L, ' '); return; }
    if (cp >= 0xC0 && cp <= 0xFF) { lay_char(L, lat[cp - 0xC0]); return; }
    // Latvian and other Latin Extended-A letters fold to their base letter
    if (cp >= 0x100 && cp <= 0x17F) {
        static const char *exta = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        if (cp - 0x100 < strlen(exta)) lay_char(L, exta[cp - 0x100]); else lay_char(L, '?');
        return;
    }
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: lay_char(L, '\''); return;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: lay_char(L, '"'); return;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: lay_char(L, '-'); return;
    case 0x2026: lay_char(L, '.'); lay_char(L, '.'); lay_char(L, '.'); return;
    case 0x2022: case 0xB7: case 0x25CF: case 0x25AA: lay_char(L, '*'); return;
    case 0xA9: lay_char(L, '('); lay_char(L, 'c'); lay_char(L, ')'); return;
    case 0xAB: lay_char(L, '<'); lay_char(L, '<'); return;
    case 0xBB: lay_char(L, '>'); lay_char(L, '>'); return;
    case 0x2192: lay_char(L, '-'); lay_char(L, '>'); return;
    case 0x2190: lay_char(L, '<'); lay_char(L, '-'); return;
    case 0x20AC: lay_char(L, 'E'); lay_char(L, 'U'); lay_char(L, 'R'); return;
    case 0xB0: lay_char(L, 'o'); return;
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: return;
    case 0x2002: case 0x2003: case 0x2009: case 0x202F: lay_char(L, ' '); return;
    }
    if (cp >= 0x80 && cp < 0xC0) return;
    lay_char(L, '?');
}

static const struct { const char *name; unsigned cp; } s_ent[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"mdash", 0x2014},
    {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"bull", 0x2022}, {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"rarr", 0x2192}, {"larr", 0x2190}, {"euro", 0x20AC},
    {"deg", 0xB0}, {"reg", 'R'}, {"trade", 'T'}, {"times", 'x'}, {"shy", 0xAD}, {"ensp", ' '}, {"emsp", ' '}, {"thinsp", ' '},
};

// Decode text (entities, UTF-8) from s[0..n) into the layout.
static void lay_text(lay_t *L, const char *s, int n)
{
    for (int i = 0; i < n;) {
        unsigned char c = s[i];
        if (c == '&') {
            int j = i + 1;
            unsigned cp = 0;
            bool ok = false;
            if (j < n && s[j] == '#') {
                j++;
                if (j < n && (s[j] == 'x' || s[j] == 'X')) { j++; while (j < n && isxdigit((unsigned char)s[j]) && j - i < 10) { cp = cp * 16 + (isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower((unsigned char)s[j]) - 'a' + 10)); j++; ok = true; } }
                else while (j < n && isdigit((unsigned char)s[j]) && j - i < 10) { cp = cp * 10 + (s[j] - '0'); j++; ok = true; }
            } else {
                int k = j;
                while (k < n && isalnum((unsigned char)s[k]) && k - j < 10) k++;
                for (int e = 0; e < (int)(sizeof(s_ent) / sizeof(s_ent[0])); e++) {
                    if ((int)strlen(s_ent[e].name) == k - j && !strncmp(s + j, s_ent[e].name, k - j)) { cp = s_ent[e].cp; ok = true; break; }
                }
                j = k;
            }
            if (ok) { if (j < n && s[j] == ';') j++; lay_codepoint(L, cp); i = j; continue; }
            lay_char(L, '&'); i++;
            continue;
        }
        if (c < 0x80) { lay_codepoint(L, c); i++; continue; }
        // UTF-8
        unsigned cp = 0xFFFD;
        int len = 1;
        if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = (c & 0x1F) << 6 | (s[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = 0x1F000; len = 4; }
        else cp = c;    // stray Latin-1 byte
        if (len == 4) { i += 4; continue; }   // emoji and the like: drop
        lay_codepoint(L, cp);
        i += len;
    }
}

// Find attribute 'name' in the tag text; copies its value (entities for &amp; decoded).
static bool get_attr(const char *tag, int n, const char *name, char *out, int out_n)
{
    int nl = strlen(name);
    for (int i = 0; i + nl < n; i++) {
        if (!isspace((unsigned char)tag[i])) continue;
        if (strncasecmp(tag + i + 1, name, nl)) continue;
        int j = i + 1 + nl;
        while (j < n && isspace((unsigned char)tag[j])) j++;
        if (j >= n || tag[j] != '=') continue;
        j++;
        while (j < n && isspace((unsigned char)tag[j])) j++;
        char q = 0;
        if (j < n && (tag[j] == '"' || tag[j] == '\'')) q = tag[j++];
        int o = 0;
        while (j < n && o < out_n - 1 && (q ? tag[j] != q : (!isspace((unsigned char)tag[j]) && tag[j] != '>'))) {
            if (tag[j] == '&' && !strncmp(tag + j, "&amp;", 5)) { out[o++] = '&'; j += 5; continue; }
            if (tag[j] != '\n' && tag[j] != '\r' && tag[j] != '\t') out[o++] = tag[j];
            j++;
        }
        out[o] = 0;
        return true;
    }
    out[0] = 0;
    return false;
}

static bool tag_is(const char *name, int n, const char *want) { return (int)strlen(want) == n && !strncasecmp(name, want, n); }
static bool tag_in(const char *name, int n, const char *list)
{
    for (const char *p = list; *p;) {
        const char *q = strchr(p, ' ');
        int l = q ? (int)(q - p) : (int)strlen(p);
        if (l == n && !strncasecmp(p, name, n)) return true;
        if (!q) break;
        p = q + 1;
    }
    return false;
}

static void lay_tag(lay_t *L, const char *tag, int n, bool closing)
{
    int nl = 0;
    while (nl < n && (isalnum((unsigned char)tag[nl]))) nl++;
    if (!nl) return;
    browser_t *b = L->b;
    char val[URL_MAX], url[URL_MAX];

    if (tag_is(tag, nl, "title")) { lay_flush_word(L); L->in_title = !closing; return; }
    if (tag_is(tag, nl, "br")) { lay_flush_word(L); lay_newline(L, true); return; }
    if (tag_is(tag, nl, "hr")) {
        lay_flush_word(L); lay_block(L, 4);
        box_t *x = box_add(b);
        if (x) { x->x = L->indent; x->w = L->width - L->indent; x->h = 8; x->style = ST_HR; x->y = L->y; }
        L->line_h = 8; L->x = L->indent + 1; lay_newline(L, false);
        return;
    }
    if (tag_in(tag, nl, "h1 h2")) { lay_flush_word(L); lay_block(L, 10); L->big += closing ? -1 : 1; if (L->big < 0) L->big = 0; if (closing) lay_block(L, 6); return; }
    if (tag_in(tag, nl, "h3 h4 h5 h6")) { lay_flush_word(L); lay_block(L, 8); L->bold += closing ? -1 : 1; if (L->bold < 0) L->bold = 0; if (closing) lay_block(L, 4); return; }
    if (tag_in(tag, nl, "p div section article header footer nav main aside figure figcaption address center table dl fieldset details summary")) {
        lay_flush_word(L); lay_block(L, tag_is(tag, nl, "p") || tag_is(tag, nl, "table") ? 7 : 0); return;
    }
    if (tag_in(tag, nl, "tr dt caption")) { lay_flush_word(L); lay_block(L, 0); return; }
    if (tag_in(tag, nl, "td th dd")) {
        lay_flush_word(L);
        if (!closing && L->x > L->indent) { L->space = true; lay_box(L, " ", 1, 0, 12); }
        if (tag_is(tag, nl, "th")) { L->bold += closing ? -1 : 1; if (L->bold < 0) L->bold = 0; }
        return;
    }
    if (tag_in(tag, nl, "b strong")) { lay_flush_word(L); L->bold += closing ? -1 : 1; if (L->bold < 0) L->bold = 0; return; }
    if (tag_in(tag, nl, "code tt kbd samp")) { lay_flush_word(L); L->mono += closing ? -1 : 1; if (L->mono < 0) L->mono = 0; return; }
    if (tag_is(tag, nl, "pre")) { lay_flush_word(L); lay_block(L, 6); L->pre = !closing; return; }
    if (tag_is(tag, nl, "blockquote")) { lay_flush_word(L); lay_block(L, 6); L->indent += closing ? -20 : 20; if (L->indent < MARGIN) L->indent = MARGIN; L->x = L->indent; return; }
    if (tag_in(tag, nl, "ul ol")) {
        lay_flush_word(L); lay_block(L, L->list_depth ? 0 : 6);
        if (!closing) {
            if (L->list_depth < 8) L->list_num[L->list_depth] = tag_is(tag, nl, "ol") ? 1 : 0;
            L->list_depth++; L->indent += 18;
        } else if (L->list_depth > 0) { L->list_depth--; L->indent -= 18; }
        if (L->indent < MARGIN) L->indent = MARGIN;
        L->x = L->indent;
        return;
    }
    if (tag_is(tag, nl, "li")) {
        lay_flush_word(L);
        if (closing) return;
        lay_block(L, 0);
        char bullet[8] = "*";
        int d = L->list_depth > 0 && L->list_depth <= 8 ? L->list_depth - 1 : -1;
        if (d >= 0 && L->list_num[d]) snprintf(bullet, sizeof(bullet), "%d.", L->list_num[d]++);
        int save = L->indent;
        L->x = L->indent - 16 < MARGIN ? MARGIN : L->indent - 16;
        int lk = L->link; L->link = -1;
        lay_box(L, bullet, strlen(bullet), 0, 0);
        L->link = lk;
        L->x = save;
        return;
    }
    if (tag_is(tag, nl, "a")) {
        lay_flush_word(L);
        if (closing) { L->link = -1; return; }
        L->link = -1;
        if (get_attr(tag, n, "href", val, sizeof(val)) && val[0]) {
            url_resolve(b->url, val, url, sizeof(url));
            if (url[0]) L->link = link_add(b, url);
        }
        return;
    }
    if (tag_is(tag, nl, "img")) {
        lay_flush_word(L);
        if (closing) return;
        char alt[80];
        get_attr(tag, n, "alt", alt, sizeof(alt));
        if (!alt[0]) { if (L->link < 0) return; strcpy(alt, "image"); }
        L->dim++;
        lay_char(L, '[');
        lay_text(L, alt, strlen(alt));
        lay_char(L, ']');
        lay_flush_word(L);
        L->dim--;
        L->space = true;
        return;
    }
    if (tag_is(tag, nl, "form")) {
        lay_flush_word(L); lay_block(L, 4);
        L->form_action[0] = 0; L->form_hidden[0] = 0; L->skip_form = false;
        if (!closing) {
            get_attr(tag, n, "action", val, sizeof(val));
            url_resolve(b->url, val[0] ? val : b->url, L->form_action, sizeof(L->form_action));
            char *q = strchr(L->form_action, '?');
            if (q) *q = 0;
            char method[12];
            get_attr(tag, n, "method", method, sizeof(method));
            L->skip_form = !strcasecmp(method, "post");
        }
        return;
    }
    if (tag_is(tag, nl, "input") && !closing && L->form_action[0] && !L->skip_form) {
        char type[16], name[64];
        get_attr(tag, n, "type", type, sizeof(type));
        get_attr(tag, n, "name", name, sizeof(name));
        if (!strcasecmp(type, "hidden")) {
            char v[96], ev[128], en[96];
            get_attr(tag, n, "value", v, sizeof(v));
            url_encode(name, en, sizeof(en));
            url_encode(v, ev, sizeof(ev));
            if (name[0] && strlen(L->form_hidden) + strlen(en) + strlen(ev) + 3 < sizeof(L->form_hidden)) {
                strlcat(L->form_hidden, en, sizeof(L->form_hidden));
                strlcat(L->form_hidden, "=", sizeof(L->form_hidden));
                strlcat(L->form_hidden, ev, sizeof(L->form_hidden));
                strlcat(L->form_hidden, "&", sizeof(L->form_hidden));
            }
        } else if (name[0] && (!type[0] || !strcasecmp(type, "text") || !strcasecmp(type, "search"))) {
            // a text box: activating it asks for the text and submits the form
            lay_flush_word(L);
            char en[96];
            url_encode(name, en, sizeof(en));
            snprintf(url, sizeof(url), "form:%s?%s%s=", L->form_action, L->form_hidden, en);
            int lk = L->link;
            L->link = link_add(b, url);
            L->space = true;
            box_t *before = b->nbox ? &b->boxes[b->nbox - 1] : NULL;
            (void)before;
            lay_box(L, " Type here and press Enter ", 27, ST_FIELD, 180);
            L->link = lk;
            L->space = true;
        }
        return;
    }
}

static void page_layout(browser_t *b, int width)
{
    page_free_layout(b);
    b->title[0] = 0;
    b->laid_w = width;
    lay_t *L = calloc(1, sizeof(lay_t));
    if (!L) return;
    L->b = b; L->width = width - MARGIN; L->indent = MARGIN; L->x = MARGIN; L->y = MARGIN; L->link = -1;
    const char *s = b->src;
    int n = b->src_len;
    if (!s) n = 0;
    if (b->plain) {
        L->pre = true;
        for (int i = 0; i < n; i++) lay_codepoint(L, (unsigned char)s[i] < 0x80 ? (unsigned char)s[i] : '?');
    } else {
        int i = 0;
        const char *skip_until = NULL;      // inside <script>, <style>, ...
        while (i < n) {
            if (s[i] != '<') {
                int j = i;
                while (j < n && s[j] != '<') j++;
                if (!skip_until) lay_text(L, s + i, j - i);
                i = j;
                continue;
            }
            if (i + 3 < n && !strncmp(s + i, "<!--", 4)) {
                const char *e = strstr(s + i + 4, "-->");
                i = e ? (int)(e - s) + 3 : n;
                continue;
            }
            int j = i + 1;
            bool closing = j < n && s[j] == '/';
            if (closing) j++;
            if (j >= n || !(isalpha((unsigned char)s[j]) || s[j] == '!' || s[j] == '?')) { if (!skip_until) lay_char(L, '<'); i++; continue; }
            int k = j;
            char q = 0;
            while (k < n && (q || s[k] != '>')) {
                if (q) { if (s[k] == q) q = 0; }
                else if (s[k] == '"' || s[k] == '\'') q = s[k];
                k++;
                if (k - j > 4000) break;
            }
            int nl = 0;
            while (j + nl < k && isalnum((unsigned char)s[j + nl])) nl++;
            if (skip_until) {
                if (closing && tag_is(s + j, nl, skip_until)) skip_until = NULL;
            } else if (!closing && tag_in(s + j, nl, "script style svg select template iframe noscript head-skip object canvas video audio")) {
                static const char *const names[] = {"script", "style", "svg", "select", "template", "iframe", "noscript", "object", "canvas", "video", "audio"};
                for (int t = 0; t < 11; t++) if (tag_is(s + j, nl, names[t])) skip_until = names[t];
                if (k > j && s[k - 1] == '/') skip_until = NULL;   // self-closed
            } else {
                lay_tag(L, s + j, k - j, closing);
            }
            i = k < n ? k + 1 : n;
        }
    }
    lay_flush_word(L);
    lay_newline(L, false);
    b->doc_h = L->y + MARGIN;
    free(L);
}

// ================================================================ view

static int view_h(browser_t *b) { return b->view->r.h - 4; }
static int view_w(browser_t *b) { return b->view->r.w - 4 - SCROLL_W; }

static void br_clamp(browser_t *b)
{
    int max = b->doc_h - view_h(b);
    if (b->scroll > max) b->scroll = max;
    if (b->scroll < 0) b->scroll = 0;
}

static int first_box_at(browser_t *b, int y)
{
    int lo = 0, hi = b->nbox;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (b->boxes[mid].y + 28 < y) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void big_text(gfx_t *g, int x, int y, const char *s, int n, pixel_t col)
{
    for (int i = 0; i < n; ) {
        int b;
        int gi = gfx_glyph(s, i, n, &b);
        for (int r = 0; r < FONT_UI_H; r++) {
            uint32_t bits = font_ui_rows[gi][r];
            bits |= bits >> 1;
            for (int k = 0; bits; k++, bits = (bits << 1) & 0xFFFF) if (bits & 0x8000) gfx_fill(g, x + k * 2, y + r * 2, 2, 2, col);
        }
        x += (font_ui_adv[gi] + 1) * 2;
        i += b;
    }
}

static void view_paint(widget_t *w, gfx_t *g)
{
    browser_t *b = w->win->user;
    gfx_fill(g, 0, 0, w->r.w, w->r.h, C_WHITE);
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    b->sb.total = b->doc_h; b->sb.page = view_h(b); b->sb.pos = b->scroll;
    rect_t sr = {w->r.w - 2 - SCROLL_W, 2, SCROLL_W, w->r.h - 4};
    scroll_paint(g, &sr, &b->sb, false);
    gfx_state_t st;
    gfx_save(g, &st);
    if (!gfx_clip(g, 2, 2, view_w(b), view_h(b))) { gfx_restore(g, &st); return; }
    int top = b->scroll, bottom = b->scroll + view_h(b);
    bool foc = ui_focused(w);
    for (int i = first_box_at(b, top); i < b->nbox; i++) {
        box_t *x = &b->boxes[i];
        if (x->y > bottom) break;
        int px = 2 + x->x, py = 2 + x->y - top;
        if (!gfx_visible(g, px, py, x->w, x->h)) continue;
        if (x->style & ST_HR) { gfx_hline(g, px, py + 3, x->w, C_DKGREY); gfx_hline(g, px, py + 4, x->w, C_GREY); continue; }
        bool sel = x->link >= 0 && x->link == b->focus_link;
        if (x->style & ST_FIELD) {
            gfx_fill(g, px, py, x->w, x->h, C_WHITE);
            ui_draw_sunken(g, px, py, x->w, x->h);
            gfx_text(g, px + 4, py + 2, b->text + x->off, x->len, FONT_UI, C_DKGREY);
            if (sel) gfx_dotted(g, px - 1, py - 1, x->w + 2, x->h + 2, foc ? C_BLACK : C_DKGREY);
            continue;
        }
        pixel_t col = (x->style & ST_LINK) ? C_BLUE : (x->style & ST_DIM) ? C_DKGREY : C_BLACK;
        // words of one link on one line are joined: the gap gets the underline / highlight too
        int gap = 0;
        if (i > 0 && x->link >= 0 && b->boxes[i - 1].link == x->link && b->boxes[i - 1].y + b->boxes[i - 1].h == x->y + x->h &&
            b->boxes[i - 1].x < x->x) gap = x->x - (b->boxes[i - 1].x + b->boxes[i - 1].w);
        if (sel) { gfx_fill(g, px - 1 - gap, py, x->w + 2 + gap, x->h, foc ? C_NAVY : C_DKGREY); col = C_WHITE; }
        if (gap && (x->style & ST_LINK) && !(x->style & ST_BIG)) gfx_hline(g, px - gap, py + (x->style & ST_MONO ? 14 : 12), gap, col);
        const char *t = b->text + x->off;
        if (x->style & ST_BIG) big_text(g, px, py + 2, t, x->len, col);
        else gfx_text(g, px, py + (x->style & ST_MONO ? 0 : 1), t, x->len, style_font(x->style), col);
        if ((x->style & ST_LINK) && !(x->style & ST_BIG)) gfx_hline(g, px, py + (x->style & ST_MONO ? 14 : 12), x->w, col);
    }
    gfx_restore(g, &st);
}

static void br_set_status(browser_t *b, const char *s) { ui_set_text(b->status, s); }

static void br_show_link(browser_t *b)
{
    if (b->focus_link >= 0 && b->focus_link < b->nlinks) {
        const char *u = b->links[b->focus_link];
        br_set_status(b, !strncmp(u, "form:", 5) ? "Search box: press Enter to type" : u);
    } else br_set_status(b, b->loading ? "Loading..." : "Done");
}

static void br_focus_link(browser_t *b, int link)
{
    b->focus_link = link;
    if (link >= 0) {
        for (int i = 0; i < b->nbox; i++) {
            if (b->boxes[i].link != link) continue;
            if (b->boxes[i].y < b->scroll + 4) b->scroll = b->boxes[i].y - 20;
            if (b->boxes[i].y + b->boxes[i].h > b->scroll + view_h(b) - 4) b->scroll = b->boxes[i].y - view_h(b) / 2;
            br_clamp(b);
            break;
        }
    }
    br_show_link(b);
    ui_invalidate(b->view);
}

static void br_follow(browser_t *b, int link)
{
    if (link < 0 || link >= b->nlinks) return;
    const char *u = b->links[link];
    if (!strncmp(u, "form:", 5)) {
        char q[96] = "", eq[200], url[URL_MAX];
        if (!inputbox(b->win, "Search", "Text to send:", q, sizeof(q)) || !q[0]) return;
        url_encode(q, eq, sizeof(eq));
        snprintf(url, sizeof(url), "%s%s", u + 5, eq);
        br_go(b, url, true);
        return;
    }
    char url[URL_MAX];
    strlcpy(url, u, sizeof(url));
    br_go(b, url, true);
}

// first link whose first box is at or below the top of the view (for Tab from nothing)
static int first_visible_link(browser_t *b, bool backwards)
{
    int best = -1;
    for (int i = 0; i < b->nbox; i++) {
        box_t *x = &b->boxes[i];
        if (x->link < 0) continue;
        bool vis = x->y + x->h > b->scroll && x->y < b->scroll + view_h(b);
        if (!vis) continue;
        if (!backwards) return x->link;
        best = x->link;
    }
    return best;
}

static bool view_key(widget_t *w, const event_t *e)
{
    browser_t *b = w->win->user;
    if (!e->down || (e->mods & (MOD_ALT | MOD_CTRL))) return false;
    int page = view_h(b) - 30;
    switch (e->key) {
    case K_UP: b->scroll -= 30; break;
    case K_DOWN: b->scroll += 30; break;
    case K_PGUP: b->scroll -= page; break;
    case K_PGDN: b->scroll += page; break;
    case K_SPACE: b->scroll += (e->mods & MOD_SHIFT) ? -page : page; break;
    case K_HOME: b->scroll = 0; break;
    case K_END: b->scroll = b->doc_h; break;
    case K_TAB: {
        if (!b->nlinks) return true;
        bool back = e->mods & MOD_SHIFT;
        int vis_first = first_visible_link(b, false), vis_last = first_visible_link(b, true);
        int n = b->focus_link;
        bool focus_visible = n >= 0 && vis_first >= 0 && n >= vis_first && n <= vis_last;
        if (!focus_visible && vis_first >= 0) n = back ? vis_last : vis_first;
        else if (n < 0) n = back ? b->nlinks - 1 : 0;
        else n = (n + (back ? b->nlinks - 1 : 1)) % b->nlinks;
        br_focus_link(b, n);
        return true;
    }
    case K_ENTER: case K_KP_ENTER: br_follow(b, b->focus_link); return true;
    default: return false;
    }
    br_clamp(b);
    ui_invalidate(w);
    return true;
}

static void view_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    browser_t *b = w->win->user;
    rect_t sr = {w->r.w - 2 - SCROLL_W, 2, SCROLL_W, w->r.h - 4};
    if (type == EV_WHEEL) { b->scroll -= btn * 45; br_clamp(b); ui_invalidate(w); return; }
    if (b->sb_drag || (type == EV_MOUSE_DOWN && btn == BTN_LEFT && rect_has(&sr, x, y))) {
        if (type == EV_MOUSE_DOWN) b->sb_drag = true;
        b->sb.total = b->doc_h; b->sb.page = view_h(b); b->sb.pos = b->scroll;
        if (scroll_mouse(&b->sb, &sr, false, type, x, y, 30)) { b->scroll = b->sb.pos; br_clamp(b); ui_invalidate(w); }
        if (type == EV_MOUSE_UP) { b->sb_drag = false; ui_invalidate(w); }
        return;
    }
    if (type != EV_MOUSE_DOWN && type != EV_MOUSE_MOVE) return;
    int dy = y - 2 + b->scroll, dx = x - 2;
    int hit = -1;
    for (int i = first_box_at(b, dy - 30); i < b->nbox; i++) {
        box_t *bx = &b->boxes[i];
        if (bx->y > dy) break;
        if (bx->link >= 0 && dx >= bx->x && dx < bx->x + bx->w && dy >= bx->y && dy < bx->y + bx->h) hit = bx->link;
    }
    if (type == EV_MOUSE_MOVE) {
        if (btn == 0 && hit != b->focus_link && hit >= 0) { b->focus_link = hit; br_show_link(b); ui_invalidate(w); }
        return;
    }
    if (btn == BTN_LEFT && hit >= 0) { b->focus_link = hit; br_follow(b, hit); }
}
static const canvas_ops_t view_ops = {view_paint, view_key, view_mouse};

// ================================================================ loading

static const char s_home[] =
    "<title>Home</title><h1>Bevel Browser</h1>"
    "<p>A small web browser for a small computer. It shows text, headings, lists and links. It does not run scripts or "
    "style sheets, so it works best with simple pages.</p>"
    "<h3>Places that work well</h3><ul>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search the web; results and pages are simplified for old computers</li>"
    "<li><a href=\"https://text.npr.org/\">NPR text edition</a> - news as plain text</li>"
    "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a> - news headlines</li>"
    "<li><a href=\"http://wttr.in/?0AT\">wttr.in</a> - the weather as text</li>"
    "<li><a href=\"https://en.m.wikipedia.org/wiki/Special:Random\">Wikipedia</a> - a random article</li>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first web page</a> - CERN, 1991</li>"
    "<li><a href=\"http://example.com/\">example.com</a> - the simplest test page</li></ul>"
    "<h3>On this computer</h3><ul>"
    "<li><a href=\"file:///disk/Documents/Welcome.txt\">Welcome.txt</a> on the drive</li></ul>"
    "<h3>Keys</h3><ul>"
    "<li><b>Tab</b> / <b>Shift+Tab</b>: next / previous link. <b>Enter</b>: follow it</li>"
    "<li><b>Arrows</b>, <b>PgUp</b>, <b>PgDn</b>, <b>Space</b>: scroll</li>"
    "<li><b>Ctrl+L</b> or <b>F6</b>: type an address, or words to search for</li>"
    "<li><b>Backspace</b> or <b>Alt+Left</b>: back. <b>Alt+Right</b>: forward. <b>Alt+Home</b>: this page</li>"
    "<li><b>F5</b>: reload. <b>Esc</b>: stop loading</li></ul>"
    "<hr><p>Network: open <b>Network</b> in the Bevel menu to join a Wi-Fi network.</p>";

static void br_set_page(browser_t *b, char *src, int len, bool plain)
{
    free(b->src);
    b->src = src; b->src_len = len; b->plain = plain;
    b->scroll = 0; b->focus_link = -1;
    page_layout(b, view_w(b));
    char t[140];
    snprintf(t, sizeof(t), "%s - Browser", b->title[0] ? b->title : b->url);
    wm_set_title(b->win, t);
    ui_textfield_set(b->url_f, b->url);
    ui_invalidate(b->view);
}

static void br_error_page(browser_t *b, const char *msg)
{
    char *h = heap_caps_malloc(600 + strlen(b->url), MALLOC_CAP_SPIRAM);
    if (!h) return;
    int n = sprintf(h, "<title>Problem loading page</title><h2>The page could not be shown</h2><p>%s</p><p><code>%s</code></p>"
                       "<p><a href=\"about:home\">Go to the home page</a></p>", msg, b->url);
    br_set_page(b, h, n, false);
}

typedef struct { int gen; char url[URL_MAX], final_url[URL_MAX], ctype[64]; char *body; int len, status; } load_t;

static void br_update_buttons(browser_t *b)
{
    ui_enable(b->b_back, b->hpos > 0);
    ui_enable(b->b_fwd, b->hpos < b->hlen - 1);
    ui_set_text(b->b_reload, b->loading ? "Stop" : "&Reload");
}

static void load_done(void *arg)
{
    load_t *l = arg;
    browser_t *b = s_br;
    if (!b || l->gen != b->gen) { free(l->body); free(l); return; }
    b->loading = false;
    wm_busy(false);
    if (l->status < 0) {
        if (l->status != -4) br_error_page(b, net_error_text(l->status));
    } else {
        if (l->final_url[0] && strcmp(l->final_url, b->url)) {
            strlcpy(b->url, l->final_url, sizeof(b->url));
            free(b->hist[b->hpos]);
            b->hist[b->hpos] = strdup(b->url);
        }
        bool is_text = !strncasecmp(l->ctype, "text/plain", 10);
        bool is_html = !l->ctype[0] || !strncasecmp(l->ctype, "text/html", 9) || !strncasecmp(l->ctype, "application/xhtml", 17);
        if (is_text || is_html) {
            if (l->status >= 400 && l->len < 20) {
                char m[80];
                snprintf(m, sizeof(m), "The server answered with error %d.", l->status);
                br_error_page(b, m);
            } else { br_set_page(b, l->body, l->len, is_text); l->body = NULL; }
        } else {
            char m[160];
            snprintf(m, sizeof(m), "This is not a text page (it is \"%.60s\", %d KB). The browser shows text pages only.", l->ctype, l->len / 1024);
            br_error_page(b, m);
        }
    }
    free(l->body);
    free(l);
    br_update_buttons(b);
    br_show_link(b);
}

static void load_task(void *arg)
{
    load_t *l = arg;
    browser_t *b = s_br;
    // wait a little for the network if it is still connecting (just after power-on)
    for (int i = 0; i < 80; i++) {
        net_status_t st;
        net_status(&st);
        if (net_online() || (st.state != NET_CONNECTING && !st.eth_link) || (b && b->cancel)) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    l->status = net_fetch(l->url, &l->body, &l->len, l->ctype, sizeof(l->ctype), l->final_url, sizeof(l->final_url), MAX_PAGE,
                          b ? &b->cancel : NULL, b ? &b->progress : NULL);
    ev_call(load_done, l);
    vTaskDelete(NULL);
}

static void br_go(browser_t *b, const char *url_in, bool push)
{
    char url[URL_MAX];
    while (*url_in == ' ') url_in++;
    if (!*url_in) return;
    if (strstr(url_in, "://") || !strncmp(url_in, "about:", 6) || !strncmp(url_in, "file:", 5)) strlcpy(url, url_in, sizeof(url));
    else if (url_in[0] == '/') snprintf(url, sizeof(url), "file://%s", url_in);
    else if (strchr(url_in, ' ') || !strchr(url_in, '.')) {
        // not an address: search for it
        char q[300];
        url_encode(url_in, q, sizeof(q));
        snprintf(url, sizeof(url), "http://frogfind.com/?q=%s", q);
    } else snprintf(url, sizeof(url), "http://%s", url_in);
    for (char *c = url; *c; c++) if (*c == ' ') *c = '+';

    b->cancel = true;       // any load in flight is abandoned
    b->gen++;
    if (b->loading) { b->loading = false; wm_busy(false); }
    strlcpy(b->url, url, sizeof(b->url));
    if (push) {
        for (int i = b->hpos + 1; i < b->hlen; i++) { free(b->hist[i]); b->hist[i] = NULL; }
        if (b->hlen && b->hpos == HIST_MAX - 1) { free(b->hist[0]); memmove(b->hist, b->hist + 1, (HIST_MAX - 1) * sizeof(char *)); b->hpos--; }
        b->hpos = b->hlen ? b->hpos + 1 : 0;
        b->hist[b->hpos] = strdup(url);
        b->hlen = b->hpos + 1;
    }
    ui_textfield_set(b->url_f, url);
    ui_focus(b->view);

    if (!strncmp(url, "about:", 6)) {
        char *h = heap_caps_malloc(sizeof(s_home), MALLOC_CAP_SPIRAM);
        if (h) { memcpy(h, s_home, sizeof(s_home)); br_set_page(b, h, sizeof(s_home) - 1, false); }
    } else if (!strncmp(url, "file://", 7)) {
        int n = 0;
        char *body = fs_read_all(url + 7, &n);
        if (!body) br_error_page(b, "There is no such file on the drive.");
        else {
            const char *ext = fs_ext(url);
            br_set_page(b, body, n, !(!strcasecmp(ext, "htm") || !strcasecmp(ext, "html")));
        }
    } else {
        load_t *l = calloc(1, sizeof(load_t));
        if (!l) return;
        net_status_t st;
        net_status(&st);
        if (!net_online() && !st.eth_link && (st.state == NET_OFF || st.state == NET_IDLE || st.state == NET_FAILED)) {
            free(l);
            br_error_page(b, net_error_text(-1));
        } else {
            l->gen = b->gen;
            strlcpy(l->url, url, sizeof(l->url));
            b->cancel = false; b->progress = 0; b->loading = true;
            wm_busy(true);
            if (xTaskCreatePinnedToCore(load_task, "fetch", 8192, l, 3, NULL, SYS_CORE) != pdPASS) {
                free(l); b->loading = false; wm_busy(false);
                br_error_page(b, net_error_text(-5));
            }
        }
    }
    br_update_buttons(b);
    br_show_link(b);
}

static void br_stop(browser_t *b)
{
    if (!b->loading) return;
    b->cancel = true; b->gen++; b->loading = false;
    wm_busy(false);
    br_update_buttons(b);
    br_set_status(b, "Stopped");
}

static void br_back(browser_t *b) { if (b->hpos > 0) { b->hpos--; br_go(b, b->hist[b->hpos], false); } }
static void br_forward(browser_t *b) { if (b->hpos < b->hlen - 1) { b->hpos++; br_go(b, b->hist[b->hpos], false); } }
static void br_reload(browser_t *b) { if (b->loading) br_stop(b); else { char u[URL_MAX]; strlcpy(u, b->url, sizeof(u)); br_go(b, u, false); } }

// ================================================================ window

enum { BC_OPEN = 1, BC_HOME, BC_BACK, BC_FWD, BC_RELOAD, BC_STOP, BC_NETWORK, BC_CLOSE, BC_SOURCE, BC_BM = 100 };
static const char *const s_bm_urls[] = {"http://frogfind.com/", "https://text.npr.org/", "https://lite.cnn.com/", "http://wttr.in/?0AT",
    "https://en.m.wikipedia.org/wiki/Special:Random", "http://info.cern.ch/hypertext/WWW/TheProject.html"};

static void on_back(widget_t *w, int c) { br_back(w->win->user); }
static void on_fwd(widget_t *w, int c) { br_forward(w->win->user); }
static void on_reload(widget_t *w, int c) { br_reload(w->win->user); }
static void on_home(widget_t *w, int c) { br_go(w->win->user, "about:home", true); }
static void on_go(widget_t *w, int c) { browser_t *b = w->win->user; char u[URL_MAX]; strlcpy(u, ui_textfield_get(b->url_f), sizeof(u)); br_go(b, u, true); }

static void br_command(window_t *w, int cmd)
{
    browser_t *b = w->user;
    switch (cmd) {
    case BC_OPEN: ui_focus(b->url_f); ui_textfield_select_all(b->url_f); break;
    case BC_HOME: br_go(b, "about:home", true); break;
    case BC_BACK: br_back(b); break;
    case BC_FWD: br_forward(b); break;
    case BC_RELOAD: br_reload(b); break;
    case BC_STOP: br_stop(b); break;
    case BC_NETWORK: app_network(NULL); break;
    case BC_CLOSE: wm_close(w); break;
    case BC_SOURCE:
        if (b->src) {
            FILE *f = fopen(DISK_ROOT "/Documents/page-source.txt", "wb");
            if (f) { fwrite(b->src, 1, b->src_len, f); fclose(f); app_editor(DISK_ROOT "/Documents/page-source.txt"); }
        }
        break;
    default:
        if (cmd >= BC_BM && cmd < BC_BM + 6) br_go(b, s_bm_urls[cmd - BC_BM], true);
    }
}

static int br_menu_state(window_t *w, int cmd)
{
    browser_t *b = w->user;
    if (cmd == BC_BACK) return b->hpos > 0 ? 0 : MI_DISABLED;
    if (cmd == BC_FWD) return b->hpos < b->hlen - 1 ? 0 : MI_DISABLED;
    if (cmd == BC_STOP) return b->loading ? 0 : MI_DISABLED;
    return 0;
}

static bool br_key(window_t *w, const event_t *e)
{
    browser_t *b = w->user;
    bool ctrl = e->mods & MOD_CTRL, alt = e->mods & MOD_ALT;
    if (ctrl && !alt && e->key == K_L) { br_command(w, BC_OPEN); return true; }
    if (!ctrl && !alt && e->key == K_F6) { br_command(w, BC_OPEN); return true; }
    if (!ctrl && !alt && e->key == K_F5) { br_reload(b); return true; }
    if (ctrl && !alt && e->key == K_R) { br_reload(b); return true; }
    if (alt && !ctrl && e->key == K_LEFT) { br_back(b); return true; }
    if (alt && !ctrl && e->key == K_RIGHT) { br_forward(b); return true; }
    if (alt && !ctrl && e->key == K_HOME) { br_go(b, "about:home", true); return true; }
    if (!ctrl && !alt && e->key == K_ESC) {
        if (b->loading) { br_stop(b); return true; }
        if (w->focus == b->url_f) { ui_textfield_set(b->url_f, b->url); ui_focus(b->view); return true; }
        return true;    // Esc never closes the browser
    }
    if (!ctrl && !alt && e->key == K_BKSP && w->focus == b->view) { br_back(b); return true; }
    if ((e->key == K_ENTER || e->key == K_KP_ENTER) && w->focus == b->url_f) { on_go(b->url_f, 0); return true; }
    return false;
}

static void br_resize(window_t *w)
{
    browser_t *b = w->user;
    int cw = wm_client_w(w), ch = wm_client_h(w);
    ui_set_rect(b->url_f, 218, 5, cw - 218 - 6, 20);
    ui_set_rect(b->view, 2, 30, cw - 4, ch - 30 - 20);
    ui_set_rect(b->status, 6, ch - 16, cw - 12, 14);
    if (b->src && view_w(b) != b->laid_w) {
        // keep the reading position roughly where it was
        int64_t frac = b->doc_h > 0 ? (int64_t)b->scroll * 1000 / b->doc_h : 0;
        int fl = b->focus_link;
        page_layout(b, view_w(b));
        b->scroll = (int)(frac * b->doc_h / 1000);
        b->focus_link = fl < b->nlinks ? fl : -1;
    }
    br_clamp(b);
}

static void br_tick(void *arg)
{
    browser_t *b = s_br;
    if (!b || !b->loading) return;
    char s[64];
    if (b->progress) snprintf(s, sizeof(s), "Loading... %d KB", b->progress / 1024 + 1);
    else snprintf(s, sizeof(s), "Contacting the server...");
    br_set_status(b, s);
}

static void br_destroy(window_t *w)
{
    browser_t *b = w->user;
    b->cancel = true;
    if (b->loading) wm_busy(false);
    timer_del(b->timer);
    page_free_layout(b);
    free(b->links); free(b->boxes); free(b->text); free(b->src);
    for (int i = 0; i < HIST_MAX; i++) free(b->hist[i]);
    s_br = NULL;
    free(b);
}

static const menu_item_t bm_file[] = {
    {"&Open address...\tCtrl+L", BC_OPEN, NULL, NULL, 0},
    {"&Reload\tF5", BC_RELOAD, NULL, NULL, 0},
    {"&Stop\tEsc", BC_STOP, NULL, NULL, 0},
    {"-", 0, NULL, NULL, 0},
    {"&View page source", BC_SOURCE, NULL, &icon16_text, 0},
    {"&Network...", BC_NETWORK, NULL, &icon16_computer, 0},
    {"-", 0, NULL, NULL, 0},
    {"&Close\tAlt+F4", BC_CLOSE, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t bm_go[] = {
    {"&Back\tAlt+Left", BC_BACK, NULL, NULL, 0},
    {"&Forward\tAlt+Right", BC_FWD, NULL, NULL, 0},
    {"&Home\tAlt+Home", BC_HOME, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t bm_marks[] = {
    {"&FrogFind search", BC_BM + 0, NULL, NULL, 0},
    {"&NPR text news", BC_BM + 1, NULL, NULL, 0},
    {"&CNN Lite", BC_BM + 2, NULL, NULL, 0},
    {"&Weather (wttr.in)", BC_BM + 3, NULL, NULL, 0},
    {"Wikipedia: &random article", BC_BM + 4, NULL, NULL, 0},
    {"The f&irst web page", BC_BM + 5, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t bm_bar[] = {{"&File", 0, bm_file, NULL, 0}, {"&Go", 0, bm_go, NULL, 0}, {"&Bookmarks", 0, bm_marks, NULL, 0}, {NULL, 0, NULL, NULL, 0}};

void app_browser(const char *arg)
{
    if (s_br) { wm_activate(s_br->win); if (arg && arg[0]) br_go(s_br, arg, true); return; }
    browser_t *b = calloc(1, sizeof(browser_t));
    window_t *w = wm_create("Browser", 600, 400, WIN_RESIZABLE);
    w->icon = &icon16_app;
    w->user = b;
    b->win = w;
    b->focus_link = -1;
    s_br = b;
    wm_set_menubar(w, bm_bar);
    b->b_back = ui_button(w, 4, 3, 48, 24, "&Back", on_back);
    b->b_fwd = ui_button(w, 54, 3, 56, 24, "For&ward", on_fwd);
    b->b_reload = ui_button(w, 112, 3, 50, 24, "&Reload", on_reload);
    b->b_home = ui_button(w, 164, 3, 46, 24, "&Home", on_home);
    b->url_f = ui_textfield(w, 218, 5, 376, "", URL_MAX - 1);
    b->view = ui_canvas(w, 2, 30, 596, 350, &view_ops, true);
    b->view->flags |= WF_WANT_TAB | WF_WANT_ENTER;
    b->view->flags &= ~WF_WANT_KEYUP;
    b->status = ui_label(w, 6, 384, 580, 14, "");
    w->on_command = br_command;
    w->on_menu_state = br_menu_state;
    w->on_key = br_key;
    w->on_resize = br_resize;
    w->on_destroy = br_destroy;
    w->min_w = 360; w->min_h = 200;
    w->focus = b->view;
    b->timer = timer_add(300, true, br_tick, NULL);
    wm_show(w);
    net_boot();
    br_go(b, (arg && arg[0]) ? arg : "about:home", true);
}
