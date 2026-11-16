// Wi-Fi station management and HTTP(S) GET. The radio is only started when a network is saved
// or the user opens the Network panel or the browser, so it costs no RAM otherwise.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#ifndef CONFIG_BEVEL_NET_OPENETH
#define CONFIG_BEVEL_NET_OPENETH 0
#endif
#if CONFIG_BEVEL_NET_OPENETH
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#endif
#include "net.h"
#include "os.h"
#include "fsutil.h"

#define WIFI_FILE DISK_ROOT "/System/wifi.ini"

static bool s_started;
static volatile int s_state = NET_OFF;
static char s_ssid[33], s_ip[16];
static int s_retries;
static bool s_want;     // we want to be connected (retry on drops)

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = 0;
        if (s_want && s_retries++ < 6) { s_state = NET_CONNECTING; esp_wifi_connect(); }
        else s_state = s_want ? NET_FAILED : NET_IDLE;
    } else if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_state = NET_CONNECTED;
    }
}

#if CONFIG_BEVEL_NET_OPENETH
// Under QEMU: the emulated OpenCores Ethernet controller instead of the radio. The "network" is
// QEMU's user-mode NAT, which hands out an address by DHCP.
static bool eth_start(void)
{
    if (esp_netif_init() != ESP_OK) return false;
    esp_event_loop_create_default();
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    if (!mac || !phy || esp_eth_driver_install(&cfg, &eth) != ESP_OK) return false;
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_event, NULL);
    strlcpy(s_ssid, "Ethernet (emulator)", sizeof(s_ssid));
    s_state = NET_CONNECTING;
    s_started = true;
    return esp_eth_start(eth) == ESP_OK;
}
#endif

bool net_start(void)
{
    if (s_started) return true;
#if CONFIG_BEVEL_NET_OPENETH
    return eth_start();
#endif
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); err = nvs_flash_init(); }
    if (err != ESP_OK) return false;
    if (esp_netif_init() != ESP_OK) return false;
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return false;
    s_started = true;
    s_state = NET_IDLE;
    return true;
}

void net_status(net_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->state = s_state;
    strlcpy(o->ssid, s_ssid, sizeof(o->ssid));
    strlcpy(o->ip, s_ip, sizeof(o->ip));
    if (s_state == NET_CONNECTED && !CONFIG_BEVEL_NET_OPENETH) { wifi_ap_record_t ap; if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) o->rssi = ap.rssi; }
}

int net_scan(net_ap_t *out, int max)
{
    if (CONFIG_BEVEL_NET_OPENETH) return 0;
    if (!net_start()) return -1;
    if (s_state == NET_CONNECTING) return 0;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return 0;
    uint16_t n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&n, recs);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) if (!strcmp(out[j].ssid, (char *)recs[i].ssid)) dup = true;
        if (dup) continue;
        strlcpy(out[k].ssid, (char *)recs[i].ssid, 33);
        out[k].rssi = recs[i].rssi;
        out[k].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    free(recs);
    return k;
}

void net_connect(const char *ssid, const char *password, bool save)
{
    if (!net_start() || CONFIG_BEVEL_NET_OPENETH) return;
    s_want = false;
    esp_wifi_disconnect();
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password ? password : "", sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_retries = 0;
    s_want = true;
    s_state = NET_CONNECTING;
    esp_wifi_connect();
    if (save) {
        FILE *f = fopen(WIFI_FILE ".tmp", "wb");
        if (f) {
            fprintf(f, "%s\n%s\n", ssid, password ? password : "");
            fclose(f);
            unlink(WIFI_FILE);
            rename(WIFI_FILE ".tmp", WIFI_FILE);
        }
    }
}

void net_disconnect(bool forget)
{
    if (CONFIG_BEVEL_NET_OPENETH) return;
    s_want = false;
    if (s_started) esp_wifi_disconnect();
    s_state = s_started ? NET_IDLE : NET_OFF;
    s_ip[0] = 0;
    if (forget) { unlink(WIFI_FILE); s_ssid[0] = 0; }
}

static void boot_task(void *arg)
{
    int n;
#if CONFIG_BEVEL_NET_OPENETH
    net_start();
    vTaskDelete(NULL);
#endif
    char *s = fs_read_all(WIFI_FILE, &n);
    if (!s && CONFIG_BEVEL_WIFI_SSID[0]) {
        // nothing saved from the Network panel: the network chosen at build time (headless boards)
        net_connect(CONFIG_BEVEL_WIFI_SSID, CONFIG_BEVEL_WIFI_PASSWORD, false);
    }
    if (s) {
        char *nl = strchr(s, '\n');
        if (nl) {
            *nl = 0;
            char *pw = nl + 1;
            char *e = strchr(pw, '\n');
            if (e) *e = 0;
            if (s[0]) net_connect(s, pw, false);
        }
        free(s);
    }
    vTaskDelete(NULL);
}

void net_boot(void)
{
    static bool tried;
    if (tried || s_state != NET_OFF) return;
    tried = true;
    if (fs_exists(WIFI_FILE) || CONFIG_BEVEL_WIFI_SSID[0] || CONFIG_BEVEL_NET_OPENETH) xTaskCreatePinnedToCore(boot_task, "net_boot", 4096, NULL, 2, NULL, SYS_CORE);
}

// ---------------------------------------------------------------- HTTP

typedef struct { char *ctype; int ctype_n; char location[512]; } hdr_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    hdr_ctx_t *h = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && h && h->ctype && !strcasecmp(e->header_key, "Content-Type")) {
        strlcpy(h->ctype, e->header_value, h->ctype_n);
    }
    if (e->event_id == HTTP_EVENT_ON_HEADER && h && !strcasecmp(e->header_key, "Location")) strlcpy(h->location, e->header_value, sizeof(h->location));
    return ESP_OK;
}

const char *net_error_text(int code)
{
    switch (code) {
    case -1: return "There is no network connection. Open Network from the Bevel menu to join a Wi-Fi network.";
    case -2: return "The server could not be reached.";
    case -3: return "The page is too large for this computer.";
    case -4: return "Stopped.";
    case -5: return "Not enough memory to load the page.";
    }
    return "The page could not be loaded.";
}

// Where a redirect leads, given the page it came from.
static void redirect_target(const char *base, const char *loc, char *out, int n)
{
    const char *p = strstr(base, "://");
    const char *host = p ? p + 3 : base;
    const char *path = strchr(host, '/');
    int origin = path ? (int)(path - base) : (int)strlen(base);
    if (strstr(loc, "://")) strlcpy(out, loc, n);
    else if (loc[0] == '/' && loc[1] == '/') snprintf(out, n, "%.*s%s", p ? (int)(p - base) + 1 : 5, base, loc);
    else if (loc[0] == '/') snprintf(out, n, "%.*s%s", origin, base, loc);
    else {
        char dir[512];
        strlcpy(dir, base, sizeof(dir));
        char *q = strchr(dir + origin, '?');
        if (q) *q = 0;
        char *slash = strrchr(dir + origin, '/');
        if (slash) slash[1] = 0; else strlcat(dir, "/", sizeof(dir));
        snprintf(out, n, "%s%s", dir, loc);
    }
}

static int fetch_once(const char *url_in, char **body, int *len, char *ctype, int ctype_n, char *final_url, int final_n,
                      int max_bytes, volatile bool *cancel, volatile int *progress)
{
    static hdr_ctx_t hc;    // one fetch at a time (the browser serialises them)
    char cur[512];
    strlcpy(cur, url_in, sizeof(cur));
    memset(&hc, 0, sizeof(hc));
    hc.ctype = ctype; hc.ctype_n = ctype_n;
    esp_http_client_config_t cfg = {
        .url = cur,
        .event_handler = http_event,
        .user_data = &hc,
        .timeout_ms = 12000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "Mozilla/5.0 (compatible; BevelBrowser/1.0; ESP32-S3)",
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -5;
    esp_http_client_set_header(c, "Accept", "text/html,text/plain,*/*;q=0.5");
    esp_http_client_set_header(c, "Accept-Encoding", "identity");
    int status = -2;
    char *buf = NULL;
    int n = 0, cap = 0;
    for (int hop = 0; hop < 7; hop++) {
        hc.location[0] = 0;
        if (cancel && *cancel) { status = -4; break; }
        if (esp_http_client_open(c, 0) != ESP_OK) { status = -2; break; }
        int64_t clen = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && hc.location[0]) {
            char next[512];
            redirect_target(cur, hc.location, next, sizeof(next));
            strlcpy(cur, next, sizeof(cur));
            if (ctype && ctype_n) ctype[0] = 0;
            esp_http_client_close(c);
            esp_http_client_set_url(c, cur);
            status = -2;
            continue;
        }
        if (clen > max_bytes) { status = -3; break; }
        cap = clen > 0 ? (int)clen + 1 : 32768;
        buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { status = -5; break; }
        for (;;) {
            if (cancel && *cancel) { status = -4; break; }
            if (n + 4096 + 1 > cap) {
                if (cap >= max_bytes) { status = -3; break; }
                cap = cap * 2 > max_bytes + 1 ? max_bytes + 1 : cap * 2;
                if (cap < n + 4097) cap = n + 4097;
                char *nb = os_grow(buf, n, cap);
                if (!nb) { status = -5; break; }
                buf = nb;
            }
            int r = esp_http_client_read(c, buf + n, 4096);
            if (r < 0) { if (n == 0) status = -2; break; }
            if (r == 0) break;
            n += r;
            if (progress) *progress = n;
            if (n > max_bytes) { status = -3; break; }
        }
        break;
    }
    if (final_url && final_n) strlcpy(final_url, cur, final_n);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status < 0) { free(buf); return status; }
    if (!buf) { buf = heap_caps_malloc(1, MALLOC_CAP_SPIRAM); n = 0; if (!buf) return -5; }
    buf[n] = 0;
    *body = buf;
    *len = n;
    return status;
}

int net_fetch(const char *url, char **body, int *len, char *ctype, int ctype_n, char *final_url, int final_n,
              int max_bytes, volatile bool *cancel, volatile int *progress)
{
    *body = NULL; *len = 0;
    if (ctype && ctype_n) ctype[0] = 0;
    if (s_state != NET_CONNECTED) return -1;
    int r = fetch_once(url, body, len, ctype, ctype_n, final_url, final_n, max_bytes, cancel, progress);
    // many sites no longer answer on plain http at all: try the secure address
    if (r == -2 && !strncasecmp(url, "http://", 7) && !(cancel && *cancel)) {
        char https[520];
        snprintf(https, sizeof(https), "https://%s", url + 7);
        r = fetch_once(https, body, len, ctype, ctype_n, final_url, final_n, max_bytes, cancel, progress);
    }
    return r;
}
