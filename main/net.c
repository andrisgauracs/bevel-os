// Networking: Wi-Fi, wired Ethernet on boards that have a PHY, and HTTP(S) GET.
//
// Wi-Fi is this chip's own radio on the ESP32-S3; on the JC1060P470-V2 the esp_wifi calls are
// forwarded to the ESP32-C6 on the module (ESP-Hosted). Either way the radio is only started when a
// network is saved or the user opens the Network panel, so it costs no RAM otherwise.
// Ethernet (an IP101GR PHY on the P4's own EMAC) is brought up at boot and needs no setup: plug in a
// cable and DHCP does the rest, and it outranks Wi-Fi as the default route. Once any link has an
// address, the clock is set from the network.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "board.h"
#include "net.h"
#include "os.h"
#include "fsutil.h"
#if BOARD_HAS_ETH
#include "esp_eth.h"
#endif

#define WIFI_FILE DISK_ROOT "/System/wifi.ini"

#if BOARD_HAS_ETH
static const char *TAG = "net";   // the Ethernet log lines below are its only users
#endif
static bool s_base, s_started, s_sntp;
static volatile int s_state = NET_OFF;
static char s_ssid[33], s_ip[16];
static int s_retries;
static bool s_want;     // we want to be connected (retry on drops)

#if BOARD_HAS_ETH
static esp_eth_handle_t s_eth;
static volatile bool s_eth_link;
static char s_eth_ip[16];
static int s_eth_mbps;
#endif

static void time_synced(struct timeval *tv) { clock_net_synced(); }

static void got_address(void)
{
    if (s_sntp) return;
    s_sntp = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = time_synced;
    esp_netif_sntp_init(&cfg);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = 0;
        if (s_want && s_retries++ < 6) { s_state = NET_CONNECTING; esp_wifi_connect(); }
        else s_state = s_want ? NET_FAILED : NET_IDLE;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_state = NET_CONNECTED;
        got_address();
#if BOARD_HAS_ETH
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed = ETH_SPEED_10M;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
        s_eth_mbps = speed == ETH_SPEED_10M ? 10 : speed == ETH_SPEED_100M ? 100 : 1000;
        s_eth_link = true;
        ESP_LOGI(TAG, "Ethernet link up, %d Mbit/s", s_eth_mbps);
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        s_eth_link = false;
        s_eth_ip[0] = 0;
        ESP_LOGI(TAG, "Ethernet link down");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_eth_ip, sizeof(s_eth_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "Ethernet address %s", s_eth_ip);
        got_address();
#endif
    }
}

// TCP/IP stack and event loop, shared by the links this board has.
static bool base_start(void)
{
    if (s_base) return true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); err = nvs_flash_init(); }
    if (err != ESP_OK) return false;
    if (esp_netif_init() != ESP_OK) return false;
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
#if BOARD_HAS_ETH
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
#endif
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    s_base = true;
    return true;
}

#if BOARD_HAS_ETH
static void eth_start(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.smi_gpio.mdc_num = BOARD_PIN_ETH_MDC;
    emac_cfg.smi_gpio.mdio_num = BOARD_PIN_ETH_MDIO;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;    // 50 MHz from the PHY
    emac_cfg.clock_config.rmii.clock_gpio = BOARD_PIN_ETH_REF_CLK;
    emac_cfg.emac_dataif_gpio.rmii = (eth_mac_rmii_gpio_config_t){
        .tx_en_num = BOARD_PIN_ETH_TX_EN,
        .txd0_num = BOARD_PIN_ETH_TXD0,
        .txd1_num = BOARD_PIN_ETH_TXD1,
        .crs_dv_num = BOARD_PIN_ETH_CRS_DV,
        .rxd0_num = BOARD_PIN_ETH_RXD0,
        .rxd1_num = BOARD_PIN_ETH_RXD1,
    };
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_cfg.reset_gpio_num = BOARD_PIN_ETH_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    esp_err_t err = ESP_FAIL;
    if (mac && phy) {
        esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
        err = esp_eth_driver_install(&cfg, &s_eth);
    }
    if (err != ESP_OK) {
        if (phy) phy->del(phy);
        if (mac) mac->del(mac);
        s_eth = NULL;
        ESP_LOGE(TAG, "Ethernet not available: %s", esp_err_to_name(err));
        return;
    }
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = 150;      // above the Wi-Fi station's 100: Ethernet is the default route
    esp_netif_config_t ncfg = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    esp_netif_t *netif = esp_netif_new(&ncfg);
    esp_netif_set_hostname(netif, "bevel");
    esp_netif_attach(netif, esp_eth_new_netif_glue(s_eth));
    esp_eth_start(s_eth);
}
#endif

bool net_online(void)
{
#if BOARD_HAS_ETH
    if (s_eth_ip[0]) return true;
#endif
    return s_state == NET_CONNECTED;
}

bool net_start(void)
{
    if (s_started) return true;
    if (!base_start()) return false;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
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
    if (s_state == NET_CONNECTED) { wifi_ap_record_t ap; if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) o->rssi = ap.rssi; }
#if BOARD_HAS_ETH
    o->eth_present = s_eth != NULL;
    o->eth_link = s_eth_link;
    o->eth_mbps = s_eth_mbps;
    strlcpy(o->eth_ip, s_eth_ip, sizeof(o->eth_ip));
#endif
}

void net_summary(char *buf, int n)
{
#if BOARD_HAS_ETH
    if (s_eth_ip[0]) { snprintf(buf, n, "Ethernet, %s", s_eth_ip); return; }
    if (s_eth_link) { snprintf(buf, n, "Ethernet, waiting for an address"); return; }
#endif
    if (s_state == NET_CONNECTED) snprintf(buf, n, "Wi-Fi \"%s\", %s", s_ssid, s_ip);
    else snprintf(buf, n, "not connected");
}

int net_scan(net_ap_t *out, int max)
{
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
    if (!net_start()) return;
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
    s_want = false;
    if (s_started) esp_wifi_disconnect();
    s_state = s_started ? NET_IDLE : NET_OFF;
    s_ip[0] = 0;
    if (forget) { unlink(WIFI_FILE); s_ssid[0] = 0; }
}

static void boot_task(void *arg)
{
    if (base_start()) {
#if BOARD_HAS_ETH
        eth_start();
#endif
    }
    int n;
    char *s = fs_read_all(WIFI_FILE, &n);
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
    if (tried) return;
    tried = true;
    xTaskCreatePinnedToCore(boot_task, "net_boot", 6144, NULL, 2, NULL, SYS_CORE);
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
    case -1: return "There is no network connection. "
#if BOARD_HAS_ETH
                    "Plug in an Ethernet cable, or "
#endif
                    "open Network from the Bevel menu to join a Wi-Fi network.";
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
        .user_agent = "Mozilla/5.0 (compatible; BevelBrowser/1.0; " OS_CHIP ")",
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
    if (!net_online()) return -1;
    int r = fetch_once(url, body, len, ctype, ctype_n, final_url, final_n, max_bytes, cancel, progress);
    // many sites no longer answer on plain http at all: try the secure address
    if (r == -2 && !strncasecmp(url, "http://", 7) && !(cancel && *cancel)) {
        char https[520];
        snprintf(https, sizeof(https), "https://%s", url + 7);
        r = fetch_once(https, body, len, ctype, ctype_n, final_url, final_n, max_bytes, cancel, progress);
    }
    return r;
}
