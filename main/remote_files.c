// Remote files: the web page's file browser, on the same server and the same Settings switch as the
// remote screen (remote.c), and like it without a password.
//
//   GET  /files                          the page (below)
//   GET  /fs/list?path=/disk/Music       the folder as JSON: {"path", "free", "total", "items": [
//                                        {"name", "dir", "size"}]}, folders first
//   GET  /fs/get?path=/disk/a.txt        the file, as a download
//   POST /fs/put?path=/disk/Music&name=x the request body becomes the file x in that folder
//   POST /fs/delete?path=...             a file, or a folder with everything in it
//   POST /fs/rename?path=...&name=y      renamed within its folder
//   POST /fs/mkdir?path=dir&name=y       a new folder
//
// Only paths under /disk are served: the drive, and the SD card at /disk/SD (which, like /disk itself,
// cannot be deleted or renamed). An upload goes to
// "name.part" first and replaces "name" only once it is complete, so a dropped connection leaves the
// old file (or nothing) behind, never half a file.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "os.h"
#include "fsutil.h"
#include "input.h"

static const char *TAG = "remote";

#define CHUNK 8192

// ---------------------------------------------------------------- the page

static const char s_files_page[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>Bevel OS files</title><style>"
"html,body{margin:0;background:#202020;color:#ccc;font:14px system-ui,sans-serif}"
"main{max-width:860px;margin:0 auto;padding:12px 16px}"
"h1{font-size:16px;font-weight:600;margin:4px 0 12px}"
"#crumbs{display:flex;flex-wrap:wrap;gap:4px;align-items:center;margin-bottom:10px}"
"#crumbs button{font:inherit;color:#ddd;background:#333;border:1px solid #555;border-radius:3px;padding:2px 8px;cursor:pointer}"
"#crumbs button:hover{background:#444}#crumbs span{color:#888}"
"table{width:100%;border-collapse:collapse}td,th{padding:5px 8px;text-align:left;border-bottom:1px solid #333}"
"th{color:#999;font-weight:500}td.n{text-align:right;color:#aaa;white-space:nowrap}"
"a{color:#9cf;text-decoration:none}a:hover{text-decoration:underline}a.dir{color:#fd8}"
"#drop{margin:14px 0;padding:14px;border:2px dashed #555;border-radius:6px;text-align:center;color:#999}"
"#drop.over{border-color:#9cf;color:#9cf}"
"#jobs div{display:flex;gap:10px;align-items:center;margin:4px 0}#jobs progress{flex:1}"
"#jobs .err{color:#f88}#free{color:#888;margin-left:auto}"
"#top{display:flex;gap:8px;align-items:center;margin-bottom:12px}#top h1{margin:0;flex:1}"
"button.act{font:inherit;font-size:12px;color:#ccc;background:#2c2c2c;border:1px solid #444;border-radius:3px;"
"padding:1px 7px;cursor:pointer;margin-left:4px}button.act:hover{background:#444}button.del:hover{background:#633}"
"#top button{font:inherit;color:#ddd;background:#333;border:1px solid #555;border-radius:3px;padding:3px 10px;cursor:pointer}"
"#top button:hover{background:#444}td.a{text-align:right;white-space:nowrap;width:1%}"
"</style></head><body><main><div id=top><button onclick=\"location.href='/'\">&lsaquo; Screen</button>"
"<h1>Bevel OS files</h1><button id=mk>New folder</button></div>"
"<div id=crumbs></div>"
"<div id=drop>Drop files here to upload them into this folder, or "
"<label style='color:#9cf;cursor:pointer'><input type=file id=pick multiple hidden>choose files</label>"
"<div id=jobs></div></div>"
"<table><thead><tr><th>Name</th><th style='text-align:right'>Size</th><th></th></tr></thead><tbody id=list></tbody></table>"
"</main><script>\n"
"let cur='/disk';const $=id=>document.getElementById(id);\n"
"const esc=s=>s.replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';');\n"
"const size=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':n<1073741824?(n/1048576).toFixed(1)+' MB':(n/1073741824).toFixed(1)+' GB';\n"
"async function show(p){\n"
" const r=await fetch('/fs/list?path='+encodeURIComponent(p),{cache:'no-store'});\n"
" if(!r.ok){$('list').innerHTML='<tr><td colspan=2>Could not read this folder.</td></tr>';return}\n"
" const j=await r.json();cur=j.path;history.replaceState(null,'','#'+encodeURIComponent(cur));\n"
// the path as buttons, the drive first ("Drive > SD > Pictures")
" const parts=cur.split('/').slice(2);let h='<button data-p=\"/disk\">Drive</button>',acc='/disk';\n"
" for(const q of parts){acc+='/'+q;h+='<span>&rsaquo;</span><button data-p=\"'+esc(acc)+'\">'+esc(q)+'</button>'}\n"
" h+='<span id=free>'+size(j.free)+' free of '+size(j.total)+'</span>';$('crumbs').innerHTML=h;\n"
" let rows=cur!='/disk'?'<tr><td><a class=dir href=# data-p=\"'+esc(cur.slice(0,cur.lastIndexOf('/')))+'\">..</a></td><td></td><td></td></tr>':'';\n"
" for(const e of j.items){const p=cur+'/'+e.name,fixed=p=='/disk/SD';\n"
"  const act=fixed?'<td></td>':'<td class=a><button class=act data-ren=\"'+esc(p)+'\">Rename</button>'\n"
"   +'<button class=\"act del\" data-del=\"'+esc(p)+'\" data-dir='+(e.dir?1:0)+'>Delete</button></td>';\n"
"  rows+=e.dir?'<tr><td><a class=dir href=# data-p=\"'+esc(p)+'\">'+esc(e.name)+'/</a></td><td></td>'+act+'</tr>'\n"
"   :'<tr><td><a href=\"/fs/get?path='+encodeURIComponent(p)+'\" download=\"'+esc(e.name)+'\">'+esc(e.name)+'</a></td><td class=n>'+size(e.size)+'</td>'+act+'</tr>'}\n"
" $('list').innerHTML=rows||'<tr><td colspan=3 style=color:#888>(empty)</td></tr>'}\n"
// delete, rename and new folder: one POST each, then the folder is listed again
"async function op(url,what){const r=await fetch(url,{method:'POST'});if(!r.ok)alert(what+': '+(await r.text()));show(cur)}\n"
"const q=(k,v)=>k+'='+encodeURIComponent(v);\n"
"document.addEventListener('click',e=>{const t=e.target,d=t.dataset||{};\n"
" if(d.p){e.preventDefault();show(d.p)}\n"
" else if(d.del){const n=d.del.slice(d.del.lastIndexOf('/')+1);\n"
"  if(confirm(d.dir=='1'?'Delete the folder \"'+n+'\" and everything in it?':'Delete \"'+n+'\"?'))op('/fs/delete?'+q('path',d.del),'Delete')}\n"
" else if(d.ren){const o=d.ren.slice(d.ren.lastIndexOf('/')+1),n=prompt('New name:',o);\n"
"  if(n&&n!=o)op('/fs/rename?'+q('path',d.ren)+'&'+q('name',n),'Rename')}});\n"
"$('mk').onclick=()=>{const n=prompt('Folder name:','New folder');if(n)op('/fs/mkdir?'+q('path',cur)+'&'+q('name',n),'New folder')};\n"
// uploads one after another, each with its own progress bar
"let queue=Promise.resolve();\n"
"function upload(files){for(const f of files){const row=document.createElement('div'),bar=document.createElement('progress');\n"
" row.textContent=f.name+' ';row.appendChild(bar);$('jobs').appendChild(row);const dir=cur;\n"
" queue=queue.then(()=>new Promise(done=>{const x=new XMLHttpRequest();\n"
"  x.open('POST','/fs/put?path='+encodeURIComponent(dir)+'&name='+encodeURIComponent(f.name));\n"
"  x.upload.onprogress=e=>{if(e.lengthComputable){bar.max=e.total;bar.value=e.loaded}};\n"
"  x.onload=()=>{if(x.status==200)row.remove();else{row.className='err';row.textContent=f.name+': '+(x.responseText||x.status)}\n"
"   if(cur==dir)show(cur);done()};\n"
"  x.onerror=()=>{row.className='err';row.textContent=f.name+': the connection failed';done()};x.send(f)}))}}\n"
"$('pick').onchange=e=>{upload(e.target.files);e.target.value=''};\n"
"const d=$('drop');d.ondragover=e=>{e.preventDefault();d.className='over'};d.ondragleave=()=>d.className='';\n"
"document.body.ondragover=e=>e.preventDefault();\n"
"document.body.ondrop=e=>{e.preventDefault();d.className='';if(e.dataTransfer.files.length)upload(e.dataTransfer.files)};\n"
"show(location.hash?decodeURIComponent(location.hash.slice(1)):'/disk');\n"
"</script></body></html>";

static esp_err_t files_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_files_page, sizeof(s_files_page) - 1);
}

// ---------------------------------------------------------------- request helpers

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void url_decode(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) { *d++ = (char)(hexval(s[1]) * 16 + hexval(s[2])); s += 2; }
        else if (*s == '+') *d++ = ' ';
        else *d++ = *s;
    }
    *d = 0;
}

// A query parameter, decoded; false if it is missing or too long.
static bool query(httpd_req_t *req, const char *key, char *out, int n)
{
    int ql = httpd_req_get_url_query_len(req);
    if (ql <= 0) return false;
    // the value arrives percent-encoded, up to three times as long as what it decodes to
    char *q = malloc(ql + 1), *raw = malloc(3 * n);
    bool ok = q && raw && httpd_req_get_url_query_str(req, q, ql + 1) == ESP_OK &&
              httpd_query_key_value(q, key, raw, 3 * n) == ESP_OK;
    if (ok) { url_decode(raw); ok = (int)strlen(raw) < n; }
    if (ok) strcpy(out, raw);
    free(q);
    free(raw);
    return ok;
}

// /disk or something below it, with no way back out; a trailing slash is dropped.
static bool safe_path(char *p)
{
    int rl = strlen(DISK_ROOT), n = strlen(p);
    while (n > rl && p[n - 1] == '/') p[--n] = 0;
    if (strncmp(p, DISK_ROOT, rl) || (p[rl] && p[rl] != '/')) return false;
    if (strstr(p, "/../") || strstr(p, "//") || (n >= 3 && !strcmp(p + n - 3, "/.."))) return false;
    return true;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

// ---------------------------------------------------------------- list

static void json_str(char *out, int n, const char *s)
{
    int k = 0;
    out[k++] = '"';
    for (; *s && k < n - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = c; }
        else if (c < 0x20) k += snprintf(out + k, n - k, "\\u%04x", c);
        else out[k++] = c;
    }
    out[k++] = '"';
    out[k] = 0;
}

static esp_err_t list_get(httpd_req_t *req)
{
    char path[256];
    if (!query(req, "path", path, sizeof(path))) strcpy(path, DISK_ROOT);
    if (!safe_path(path)) return fail(req, "403 Forbidden", "Only files under /disk.");
    fs_entry_t *ents = NULL;
    int n = fs_list(path, &ents);
    if (n < 0) return fail(req, "404 Not Found", "No such folder.");
    uint64_t total = 0, free_b = 0;
    storage_df_path(path, &total, &free_b);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[400], name[200];
    json_str(name, sizeof(name), path);
    snprintf(buf, sizeof(buf), "{\"path\":%s,\"free\":%llu,\"total\":%llu,\"items\":[", name,
             (unsigned long long)free_b, (unsigned long long)total);
    esp_err_t err = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    for (int i = 0; i < n && err == ESP_OK; i++) {
        json_str(name, sizeof(name), ents[i].name);
        snprintf(buf, sizeof(buf), "%s{\"name\":%s,\"dir\":%s,\"size\":%u}", i ? "," : "", name,
                 ents[i].is_dir ? "true" : "false", (unsigned)ents[i].size);
        err = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    free(ents);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "]}", 2);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

// ---------------------------------------------------------------- download

static esp_err_t get_get(httpd_req_t *req)
{
    char path[256];
    if (!query(req, "path", path, sizeof(path)) || !safe_path(path)) return fail(req, "403 Forbidden", "Only files under /disk.");
    FILE *f = fopen(path, "rb");
    if (!f || fs_is_dir(path)) { if (f) fclose(f); return fail(req, "404 Not Found", "No such file."); }
    // the name for the browser's save, in the form that allows any character
    char disp[400] = "attachment; filename*=UTF-8''";
    int k = strlen(disp);
    for (const char *s = fs_basename(path); *s && k < (int)sizeof(disp) - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr("-._~", c)) disp[k++] = c;
        else k += snprintf(disp + k, sizeof(disp) - k, "%%%02X", c);
    }
    disp[k] = 0;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL);
    if (!buf) { fclose(f); return fail(req, "500 Internal Server Error", "No memory."); }
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) err = httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    free(buf);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

// ---------------------------------------------------------------- upload

static void refresh_files(void *arg) { extern void files_refresh_all(void); files_refresh_all(); }

static esp_err_t put_post(httpd_req_t *req)
{
    char dir[256], name[80], path[340], part[350];
    if (!query(req, "path", dir, sizeof(dir)) || !safe_path(dir)) return fail(req, "403 Forbidden", "Only folders under /disk.");
    if (!query(req, "name", name, sizeof(name)) || !fs_valid_name(name) || strlen(name) > 67)
        return fail(req, "400 Bad Request", "That file name is not allowed here.");
    if (!fs_is_dir(dir)) return fail(req, "404 Not Found", "No such folder.");
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (fs_is_dir(path)) return fail(req, "409 Conflict", "There is a folder with that name.");
    uint64_t total = 0, free_b = 0;
    if (storage_df_path(dir, &total, &free_b) && (uint64_t)req->content_len + 65536 > free_b)
        return fail(req, "507 Insufficient Storage", "Not enough room on that drive.");
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) return fail(req, "500 Internal Server Error", "Could not create the file.");
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL);
    if (!buf) { fclose(f); unlink(part); return fail(req, "500 Internal Server Error", "No memory."); }
    size_t left = req->content_len;
    bool ok = true;
    int timeouts = 0;
    while (left > 0 && ok) {
        int r = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) { ok = false; break; }
        timeouts = 0;
        ok = fwrite(buf, 1, r, f) == (size_t)r;
        left -= r;
    }
    free(buf);
    if (fclose(f) != 0) ok = false;
    if (ok) { unlink(path); ok = rename(part, path) == 0; }
    if (!ok) {
        unlink(part);
        ESP_LOGW(TAG, "upload of %s failed", path);
        return fail(req, "500 Internal Server Error", "The upload did not finish.");
    }
    ESP_LOGI(TAG, "uploaded %s (%u bytes)", path, (unsigned)req->content_len);
    ev_call(refresh_files, NULL);           // open Files windows show it
    return httpd_resp_sendstr(req, "OK");
}

// ---------------------------------------------------------------- delete, rename, new folder

// The drive and the card's mount point are not files to delete or rename.
static bool changeable(const char *path)
{
    return strcmp(path, DISK_ROOT) && strcmp(path, SD_ROOT);
}

static esp_err_t delete_post(httpd_req_t *req)
{
    char path[256];
    if (!query(req, "path", path, sizeof(path)) || !safe_path(path) || !changeable(path))
        return fail(req, "403 Forbidden", "That cannot be deleted.");
    if (!fs_exists(path)) return fail(req, "404 Not Found", "It is not there any more.");
    if (!fs_delete(path)) return fail(req, "500 Internal Server Error", "Could not delete it.");
    ESP_LOGI(TAG, "deleted %s", path);
    ev_call(refresh_files, NULL);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t rename_post(httpd_req_t *req)
{
    char path[256], name[80], to[340];
    if (!query(req, "path", path, sizeof(path)) || !safe_path(path) || !changeable(path))
        return fail(req, "403 Forbidden", "That cannot be renamed.");
    if (!query(req, "name", name, sizeof(name)) || !fs_valid_name(name) || strlen(name) > 67)
        return fail(req, "400 Bad Request", "That name is not allowed.");
    if (!fs_exists(path)) return fail(req, "404 Not Found", "It is not there any more.");
    int dl = strrchr(path, '/') - path;
    snprintf(to, sizeof(to), "%.*s/%s", dl, path, name);
    if (!strcmp(to, path)) return httpd_resp_sendstr(req, "OK");
    // a change of capitals only is the same file on FAT, and allowed
    if (fs_exists(to) && strcasecmp(to, path)) return fail(req, "409 Conflict", "Something with that name is already there.");
    if (rename(path, to) != 0) return fail(req, "500 Internal Server Error", "Could not rename it.");
    ESP_LOGI(TAG, "renamed %s to %s", path, name);
    ev_call(refresh_files, NULL);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t mkdir_post(httpd_req_t *req)
{
    char dir[256], name[80], path[340];
    if (!query(req, "path", dir, sizeof(dir)) || !safe_path(dir) || !fs_is_dir(dir))
        return fail(req, "403 Forbidden", "Only folders under /disk.");
    if (!query(req, "name", name, sizeof(name)) || !fs_valid_name(name) || strlen(name) > 67)
        return fail(req, "400 Bad Request", "That name is not allowed.");
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (fs_exists(path)) return fail(req, "409 Conflict", "Something with that name is already there.");
    if (mkdir(path, 0777) != 0) return fail(req, "500 Internal Server Error", "Could not create the folder.");
    ev_call(refresh_files, NULL);
    return httpd_resp_sendstr(req, "OK");
}

// ---------------------------------------------------------------- registration

void remote_files_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/files", .method = HTTP_GET, .handler = files_page_get},
        {.uri = "/fs/list", .method = HTTP_GET, .handler = list_get},
        {.uri = "/fs/get", .method = HTTP_GET, .handler = get_get},
        {.uri = "/fs/put", .method = HTTP_POST, .handler = put_post},
        {.uri = "/fs/delete", .method = HTTP_POST, .handler = delete_post},
        {.uri = "/fs/rename", .method = HTTP_POST, .handler = rename_post},
        {.uri = "/fs/mkdir", .method = HTTP_POST, .handler = mkdir_post},
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) httpd_register_uri_handler(srv, &uris[i]);
}
