// Network: shows the Ethernet link and joins a Wi-Fi network. The password is typed here, on the
// computer's own keyboard.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "net.h"

#define MAX_AP 20
static window_t *s_win;
static widget_t *s_list, *s_status, *s_eth, *s_scan_btn;
static net_ap_t s_aps[MAX_AP];
static char s_sig[MAX_AP][12];
static int s_nap, s_timer;
static bool s_scanning;

static void net_get(widget_t *lv, int i, lv_item_t *it)
{
    it->text = s_aps[i].ssid;
    int q = 2 * (s_aps[i].rssi + 100);
    if (q > 100) q = 100;
    if (q < 0) q = 0;
    snprintf(s_sig[i], sizeof(s_sig[i]), "%d%%", q);
    it->col[0] = s_sig[i];
    it->col[1] = s_aps[i].secure ? "Password" : "Open";
}

static void refresh_status(void *arg)
{
    if (!s_win) return;
    net_status_t st;
    net_status(&st);
    char t[120];
    switch (st.state) {
    case NET_CONNECTED: snprintf(t, sizeof(t), "Wi-Fi: connected to %s    address %s    signal %d dBm", st.ssid, st.ip, st.rssi); break;
    case NET_CONNECTING: snprintf(t, sizeof(t), "Wi-Fi: connecting to %s...", st.ssid); break;
    case NET_FAILED: snprintf(t, sizeof(t), "Wi-Fi: could not connect to %s. Check the password and try again.", st.ssid); break;
    case NET_OFF: snprintf(t, sizeof(t), "Wi-Fi is off. Press Scan to look for networks."); break;
    default: snprintf(t, sizeof(t), s_scanning ? "Wi-Fi: looking for networks..." : "Wi-Fi: not connected."); break;
    }
    char e[120];
    if (st.eth_ip[0]) snprintf(e, sizeof(e), "Ethernet: connected at %d Mbit/s    address %s", st.eth_mbps, st.eth_ip);
    else if (st.eth_link) snprintf(e, sizeof(e), "Ethernet: cable connected, waiting for an address...");
    else snprintf(e, sizeof(e), st.eth_present ? "Ethernet: no cable." : "Ethernet: not available.");
    ui_set_text(s_eth, e);
    ui_set_text(s_status, t);
}

static void scan_done(void *arg)
{
    s_scanning = false;
    wm_busy(false);
    if (!s_win) return;
    ui_enable(s_scan_btn, true);
    ui_listview_set_count(s_list, s_nap > 0 ? s_nap : 0);
    refresh_status(NULL);
    if (s_nap < 0) msgbox(s_win, "Network", "The Wi-Fi radio could not be started.", MB_ERROR, MB_OK);
}

static void scan_task(void *arg)
{
    static net_ap_t found[MAX_AP];
    int n = net_scan(found, MAX_AP);
    memcpy(s_aps, found, sizeof(found));
    s_nap = n;
    ev_call(scan_done, NULL);
    vTaskDelete(NULL);
}

static void do_scan(widget_t *b, int code)
{
    if (s_scanning) return;
    s_scanning = true;
    wm_busy(true);
    ui_enable(s_scan_btn, false);
    refresh_status(NULL);
    xTaskCreatePinnedToCore(scan_task, "wifi_scan", 4096, NULL, 3, NULL, SYS_CORE);
}

static void do_connect(widget_t *b, int code)
{
    int i = ui_listview_sel(s_list);
    if (i < 0 || i >= s_nap) { msgbox(s_win, "Network", "Choose a network from the list first (press Scan to fill it).", MB_INFO, MB_OK); return; }
    char pw[65] = "";
    if (s_aps[i].secure) {
        char prompt[80];
        snprintf(prompt, sizeof(prompt), "Password for %s:", s_aps[i].ssid);
        if (!inputbox_password(s_win, "Wi-Fi password", prompt, pw, sizeof(pw))) return;
    }
    net_connect(s_aps[i].ssid, pw, true);
    memset(pw, 0, sizeof(pw));
    refresh_status(NULL);
}

static void do_disconnect(widget_t *b, int code)
{
    if (msgbox(s_win, "Network", "Disconnect and forget the saved network?", MB_ASK, MB_YESNO) == ID_YES) net_disconnect(true);
    refresh_status(NULL);
}

static void do_close(widget_t *b, int code) { wm_close(b->win); }
static void list_action(widget_t *lv, int code) { if (code == UI_ACTIVATE) do_connect(lv, 0); }
static void on_destroy(window_t *w) { timer_del(s_timer); s_win = NULL; }

void app_network(const char *arg)
{
    if (s_win) { wm_activate(s_win); return; }
    window_t *w = wm_create("Network", 440, 282, WIN_DIALOG);
    w->icon = &icon16_computer;
    s_win = w;
    s_eth = ui_label(w, 10, 10, 420, 14, "");
    s_status = ui_label(w, 10, 30, 420, 14, "");
    ui_label(w, 10, 52, 200, 14, "Wi-Fi &networks in range:");
    s_list = ui_listview(w, 10, 68, 420, 170, net_get);
    static const char *const titles[] = {"Name", "Signal", "Security"};
    static const int widths[] = {250, 70, 80};
    ui_listview_columns(s_list, 3, titles, widths);
    ui_listview_set_count(s_list, s_nap > 0 ? s_nap : 0);
    s_list->on_action = list_action;
    s_scan_btn = ui_button(w, 10, 248, 70, 24, "&Scan", do_scan);
    ui_button(w, 86, 248, 80, 24, "&Connect", do_connect);
    ui_button(w, 172, 248, 90, 24, "&Disconnect", do_disconnect);
    widget_t *c = ui_button(w, 356, 248, 74, 24, "Close", do_close);
    w->cancel_btn = c;
    w->on_destroy = on_destroy;
    w->focus = s_list;
    s_timer = timer_add(1000, true, refresh_status, NULL);
    refresh_status(NULL);
    wm_show(w);
    if (s_nap <= 0 && !net_online()) do_scan(NULL, 0);
}
