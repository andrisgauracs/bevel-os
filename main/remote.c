// Remote screen: a small web server that shows the screen in a browser on another machine and takes
// that browser's mouse and keyboard as if they were plugged in. Switched on and off in Settings; there
// is no password, so anyone on the network can use it while it is on.
//
//   GET  /         the page (below): a canvas, and a script that keeps it up to date
//   GET  /fb?v=N   the screen tiles that changed since version N (all of them for N = 0)
//   POST /in       input events, one per line: "m x y" pointer to, "b mask" buttons (BTN_*),
//                  "w d" wheel, "k usage 1|0" key down/up (USB HID usage, so the board's own
//                  keyboard layout decides what a key types)
//   /files, /fs/... the drive and the SD card: list, download, upload (remote_files.c)
//
// The screen is cut into 32x32 tiles. Each /fb request takes a fresh snapshot (at most every 30 ms),
// compares it with the previous one and gives every changed tile the next version number; the reply
// carries the tiles newer than what the browser has, run-length coded. The HTTP server runs all
// requests in its one task, so nothing here needs a lock.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display.h"
#include "input.h"
#include "net.h"
#include "os.h"

static const char *TAG = "remote";

void remote_files_register(httpd_handle_t srv);     // remote_files.c

#define TILE 32
#define TCOLS ((SCREEN_W + TILE - 1) / TILE)
#define TROWS ((SCREEN_H + TILE - 1) / TILE)
#define NTILES (TCOLS * TROWS)

static httpd_handle_t s_srv;
static bool s_want, s_stack_ready;
static uint16_t *s_shot, *s_last;   // new snapshot, and the one the tile versions describe
static uint32_t s_tver[NTILES];
static uint32_t s_ver, s_shot_ms;

// ---------------------------------------------------------------- the page

static const char s_page[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>Bevel OS</title><style>"
"html,body{margin:0;height:100%;background:#202020;color:#bbb;font:13px system-ui,sans-serif}"
"#bar{height:28px;line-height:28px;padding:0 10px;display:flex;gap:8px;align-items:center}"
"#bar span.grow{flex:1}"
"button{font:inherit;color:#ddd;background:#3a3a3a;border:1px solid #555;border-radius:3px;padding:2px 10px;cursor:pointer}"
"button:hover{background:#4a4a4a}"
"canvas{display:block;margin:0 auto;max-width:100%;max-height:calc(100vh - 28px);cursor:crosshair;"
"outline:none;touch-action:none;image-rendering:pixelated}"
"</style></head><body><div id=bar><span>Bevel OS &middot; <span id=st>connecting</span> &middot; "
"click the screen to type into it</span><span class=grow></span><button onclick=\"window.open('/files')\">Files</button>"
"<button id=shot>Save screenshot</button></div>"
"<canvas id=s tabindex=0></canvas><script>\n"
"const c=document.getElementById('s'),g=c.getContext('2d'),st=document.getElementById('st');\n"
"let W=0,H=0,T=0,img=null,ver=0;\n"
"const sleep=ms=>new Promise(r=>setTimeout(r,ms));\n"
// the canvas already holds the whole screen at its real size: save it as a PNG named by the time
"document.getElementById('shot').onclick=()=>{if(!W)return;c.toBlob(b=>{const a=document.createElement('a'),\n"
" d=new Date(),p=n=>String(n).padStart(2,'0');a.href=URL.createObjectURL(b);\n"
" a.download='bevel-'+d.getFullYear()+p(d.getMonth()+1)+p(d.getDate())+'-'+p(d.getHours())+p(d.getMinutes())+p(d.getSeconds())+'.png';\n"
" a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)},'image/png')};\n"
"async function poll(){for(;;){try{\n"
" const r=await fetch('/fb?v='+ver,{cache:'no-store'});if(!r.ok)throw 0;\n"
" const b=new DataView(await r.arrayBuffer());\n"
" const v=b.getUint32(0,true),w=b.getUint16(4,true),h=b.getUint16(6,true),t=b.getUint16(8,true);\n"
" if(w!=W||h!=H||!img){W=w;H=h;T=t;c.width=w;c.height=h;img=g.createImageData(w,h);if(ver){ver=0;continue}}\n"
" const d=img.data,cols=Math.ceil(W/T);let p=10,n=0,x0=W,y0=H,x1=0,y1=0;\n"
" while(p+2<=b.byteLength){const i=b.getUint16(p,true);p+=2;if(i==65535)break;\n"
"  const tx=(i%cols)*T,ty=Math.floor(i/cols)*T,tw=Math.min(T,W-tx),th=Math.min(T,H-ty),tot=tw*th;\n"
"  for(let k=0;k<tot;){const run=b.getUint8(p),q=b.getUint16(p+1,true);p+=3;\n"
"   const R=(q>>8&248)|(q>>13),G=(q>>3&252)|(q>>9&3),B=(q<<3&248)|(q>>2&7);\n"
"   for(let j=0;j<run&&k<tot;j++,k++){const o=((ty+(k/tw|0))*W+tx+k%tw)*4;d[o]=R;d[o+1]=G;d[o+2]=B;d[o+3]=255}}\n"
"  n++;x0=Math.min(x0,tx);y0=Math.min(y0,ty);x1=Math.max(x1,tx+tw);y1=Math.max(y1,ty+th)}\n"
" if(n)g.putImageData(img,0,0,x0,y0,x1-x0,y1-y0);\n"
" ver=v;st.textContent='connected';if(!n)await sleep(60);\n"
"}catch(e){st.textContent='not connected, retrying';await sleep(1000)}}}\n"
// input: pointer moves are merged while a request is under way; everything else is kept in order
"let q=[],move=null,busy=false,btns=0;const keys=new Set();\n"
"async function flush(){if(busy)return;busy=true;\n"
" while(q.length||move){if(move){q.push(move);move=null}const body=q.join('\\n');q=[];\n"
"  try{await fetch('/in',{method:'POST',body})}catch(e){}}busy=false}\n"
"function send(s){if(move){q.push(move);move=null}q.push(s);flush()}\n"
"function at(e){const r=c.getBoundingClientRect();\n"
" return Math.max(0,Math.min(W-1,Math.round((e.clientX-r.left)*W/r.width)))+' '+\n"
"  Math.max(0,Math.min(H-1,Math.round((e.clientY-r.top)*H/r.height)))}\n"
"const bit=b=>b==0?1:b==2?2:b==1?4:0;\n"
"c.addEventListener('pointermove',e=>{move='m '+at(e);flush()});\n"
"c.addEventListener('pointerdown',e=>{c.focus();c.setPointerCapture(e.pointerId);e.preventDefault();\n"
" send('m '+at(e));btns|=bit(e.button);send('b '+btns)});\n"
"c.addEventListener('pointerup',e=>{send('m '+at(e));btns&=~bit(e.button);send('b '+btns)});\n"
"c.addEventListener('contextmenu',e=>e.preventDefault());\n"
// a wheel notch is about 100 pixels of deltaY; a touchpad sends many small ones, added up here
"let acc=0;c.addEventListener('wheel',e=>{e.preventDefault();acc+=e.deltaMode?e.deltaY*33:e.deltaY;\n"
" while(acc<=-100){send('w 1');acc+=100}while(acc>=100){send('w -1');acc-=100}},{passive:false});\n"
// KeyboardEvent.code (the physical key) to USB HID usage
"const U={Enter:40,Escape:41,Backspace:42,Tab:43,Space:44,Minus:45,Equal:46,BracketLeft:47,BracketRight:48,\n"
" Backslash:49,Semicolon:51,Quote:52,Backquote:53,Comma:54,Period:55,Slash:56,CapsLock:57,PrintScreen:70,\n"
" ScrollLock:71,Pause:72,Insert:73,Home:74,PageUp:75,Delete:76,End:77,PageDown:78,ArrowRight:79,ArrowLeft:80,\n"
" ArrowDown:81,ArrowUp:82,NumLock:83,NumpadDivide:84,NumpadMultiply:85,NumpadSubtract:86,NumpadAdd:87,\n"
" NumpadEnter:88,Numpad0:98,NumpadDecimal:99,IntlBackslash:100,ContextMenu:101,ControlLeft:224,ShiftLeft:225,\n"
" AltLeft:226,MetaLeft:227,ControlRight:228,ShiftRight:229,AltRight:230,MetaRight:231};\n"
"for(let i=0;i<26;i++)U['Key'+String.fromCharCode(65+i)]=4+i;\n"
"for(let i=1;i<10;i++){U['Digit'+i]=29+i;U['Numpad'+i]=88+i}U.Digit0=39;\n"
"for(let i=1;i<13;i++)U['F'+i]=57+i;\n"
"c.addEventListener('keydown',e=>{const u=U[e.code];if(!u)return;e.preventDefault();\n"
" if(!keys.has(u)){keys.add(u);send('k '+u+' 1')}});\n"
"c.addEventListener('keyup',e=>{const u=U[e.code];if(!u)return;e.preventDefault();\n"
" if(keys.delete(u))send('k '+u+' 0')});\n"
// leaving the page or the canvas lets go of everything held, so no key or button stays stuck
"function release(){for(const u of keys)send('k '+u+' 0');keys.clear();if(btns){btns=0;send('b 0')}}\n"
"c.addEventListener('blur',release);window.addEventListener('blur',release);\n"
"poll();\n"
"</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_page, sizeof(s_page) - 1);
}

// ---------------------------------------------------------------- screen tiles

static void take_snapshot(void)
{
    uint32_t now = os_uptime_ms();
    if (s_ver && now - s_shot_ms < 30) return;          // several browsers share one snapshot
    s_shot_ms = now;
    disp_snapshot(s_shot, false);                       // with the pointer, so the browser sees it
    if (!s_ver) {
        s_ver = 1;
        for (int t = 0; t < NTILES; t++) s_tver[t] = 1;
    } else {
        bool any = false;
        for (int t = 0; t < NTILES; t++) {
            int tx = (t % TCOLS) * TILE, ty = (t / TCOLS) * TILE;
            int tw = SCREEN_W - tx < TILE ? SCREEN_W - tx : TILE, th = SCREEN_H - ty < TILE ? SCREEN_H - ty : TILE;
            for (int y = 0; y < th; y++) {
                size_t o = (size_t)(ty + y) * SCREEN_W + tx;
                if (memcmp(s_shot + o, s_last + o, tw * 2)) {
                    if (!any) { any = true; s_ver++; }
                    s_tver[t] = s_ver;
                    break;
                }
            }
        }
    }
    uint16_t *t = s_last; s_last = s_shot; s_shot = t;
}

typedef struct { httpd_req_t *req; uint8_t buf[4096]; int n; esp_err_t err; } out_t;

static void out_put(out_t *o, const void *p, int n)
{
    if (o->err != ESP_OK) return;
    if (o->n + n > (int)sizeof(o->buf)) {
        o->err = httpd_resp_send_chunk(o->req, (const char *)o->buf, o->n);
        o->n = 0;
    }
    memcpy(o->buf + o->n, p, n);
    o->n += n;
}

static esp_err_t fb_get(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[32], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "v", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);
    take_snapshot();
    if (since > s_ver) since = 0;                       // the board restarted since: send everything
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    static out_t o;
    o.req = req; o.n = 0; o.err = ESP_OK;
    uint8_t hdr[10];
    uint32_t ver = s_ver;
    memcpy(hdr, &ver, 4);
    uint16_t dims[3] = {SCREEN_W, SCREEN_H, TILE};
    memcpy(hdr + 4, dims, 6);
    out_put(&o, hdr, sizeof(hdr));
    for (int t = 0; t < NTILES; t++) {
        if (s_tver[t] <= since) continue;
        uint16_t idx = t;
        out_put(&o, &idx, 2);
        int tx = (t % TCOLS) * TILE, ty = (t / TCOLS) * TILE;
        int tw = SCREEN_W - tx < TILE ? SCREEN_W - tx : TILE, th = SCREEN_H - ty < TILE ? SCREEN_H - ty : TILE;
        // runs of one colour, row after row through the tile: count (1..255), pixel (RGB565)
        int tot = tw * th, k = 0;
        while (k < tot) {
            uint16_t px = s_last[(size_t)(ty + k / tw) * SCREEN_W + tx + k % tw];
            int run = 1;
            while (k + run < tot && run < 255 && s_last[(size_t)(ty + (k + run) / tw) * SCREEN_W + tx + (k + run) % tw] == px) run++;
            uint8_t rec[3] = {(uint8_t)run, (uint8_t)px, (uint8_t)(px >> 8)};
            out_put(&o, rec, 3);
            k += run;
        }
    }
    uint16_t end = 0xFFFF;
    out_put(&o, &end, 2);
    if (o.err == ESP_OK && o.n) o.err = httpd_resp_send_chunk(req, (const char *)o.buf, o.n);
    if (o.err == ESP_OK) o.err = httpd_resp_send_chunk(req, NULL, 0);
    return o.err;
}

// ---------------------------------------------------------------- input

static esp_err_t in_post(httpd_req_t *req)
{
    char body[1024];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1, got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; return ESP_FAIL; }
        got += r;
    }
    body[got] = 0;
    for (char *line = strtok(body, "\n"); line; line = strtok(NULL, "\n")) {
        int a, b;
        switch (line[0]) {
        case 'm': if (sscanf(line + 1, "%d %d", &a, &b) == 2 && a >= 0 && b >= 0 && a < SCREEN_W && b < SCREEN_H) input_mouse_abs(a, b); break;
        case 'b': if (sscanf(line + 1, "%d", &a) == 1) input_mouse_buttons((uint8_t)(a & (BTN_LEFT | BTN_RIGHT | BTN_MID))); break;
        case 'w': if (sscanf(line + 1, "%d", &a) == 1 && a) input_wheel(a > 0 ? 1 : -1); break;
        case 'k': if (sscanf(line + 1, "%d %d", &a, &b) == 2 && a > 3 && a < 256) input_key((uint8_t)a, b != 0); break;
        }
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------- on / off

static void start(void)
{
    if (s_srv || !s_stack_ready) return;
    size_t px = (size_t)SCREEN_W * SCREEN_H;
    if (!s_shot) s_shot = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    if (!s_last) s_last = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    if (!s_shot || !s_last) { ESP_LOGE(TAG, "no memory for the screen copies"); return; }
    s_ver = 0;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.core_id = SYS_CORE;
    cfg.task_priority = 3;
    cfg.lru_purge_enable = true;    // a browser that went away without closing frees its socket
    cfg.max_uri_handlers = 12;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "the web server did not start"); s_srv = NULL; return; }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/fb", .method = HTTP_GET, .handler = fb_get},
        {.uri = "/in", .method = HTTP_POST, .handler = in_post},
    };
    for (int i = 0; i < 3; i++) httpd_register_uri_handler(s_srv, &uris[i]);
    remote_files_register(s_srv);
    ESP_LOGI(TAG, "web access on, port %d", cfg.server_port);
}

static void stop(void)
{
    if (!s_srv) return;
    httpd_stop(s_srv);
    s_srv = NULL;
    ESP_LOGI(TAG, "web access off");
}

// Starting and stopping happen in a task of their own, never in the caller: httpd_stop waits for the
// server's task to finish the request it is on, and with the system core busy (the camera, a large
// copy) that can take a while. Settings, which switches it from the GUI task, must not hang on that.
static TaskHandle_t s_ctl;

static void ctl_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_want && s_stack_ready) start();
        else if (!s_want) stop();
    }
}

static void poke(void)
{
    if (!s_ctl) xTaskCreatePinnedToCore(ctl_task, "remote_ctl", 3072, NULL, 3, &s_ctl, SYS_CORE);
    if (s_ctl) xTaskNotifyGive(s_ctl);
}

void remote_set(bool on)
{
    s_want = on;
    if (on || s_srv) poke();
}

bool remote_on(void) { return s_want; }

void remote_stack_ready(void)
{
    s_stack_ready = true;
    if (s_want) poke();
}
