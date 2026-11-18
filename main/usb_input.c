// USB host HID input on the board's host port: the ESP32-P4's high-speed USB 2.0 OTG controller and
// the dock plugged into it, or the ESP32-S3's full-speed controller on GPIO19/20 behind a USB-C
// breakout (docs/HANDOFF-AMENDMENT-USB.md). Report descriptors are parsed, so keyboards, relative
// mice and absolute pointers (KVMs, tablets) all work, with or without report IDs.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "input.h"
#include "display.h"
#include "os.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "soc/usb_dwc_struct.h"
#endif

static const char *TAG = "usb";

#if CONFIG_IDF_TARGET_ESP32P4
// The host port is the ESP32-P4's high-speed controller, but ESP-IDF's host stack has no transaction
// translator support: a keyboard or mouse (low/full speed) behind a high-speed hub is refused
// ("Connected device is FS, transaction translator (TT) is not supported"). Keyboards and mice are
// all this OS uses the port for, so the controller is told to run the port at full speed only
// (HCFG.FSLSSupp: it then does not answer the high-speed chirp). Hubs come up as USB 1.1 hubs, and
// everything behind them works. Set to 0 to get high speed back (devices straight on the port only).
#define USB_FORCE_FULL_SPEED 1

static inline void force_full_speed(void)
{
#if USB_FORCE_FULL_SPEED
    // a port recovery resets the controller and with it this bit, so it is re-applied by the watchdog task
    if (!USB_DWC_HS.hcfg_reg.fslssupp) USB_DWC_HS.hcfg_reg.fslssupp = 1;
#endif
}
#else
// The S3's controller only runs full speed, so there is nothing to force.
static inline void force_full_speed(void) { }
#endif

enum { F_KEY_MODS, F_KEY_ARRAY, F_BUTTONS, F_X, F_Y, F_WHEEL };
typedef struct {
    uint8_t kind, report_id;
    uint16_t bit_off;
    uint8_t bits, count;
    bool relative;
    int32_t lmin, lmax;
    uint8_t usage_min;      // F_KEY_MODS: first modifier usage; F_BUTTONS: first button number
} field_t;

#define MAX_FIELDS 12
#define MAX_IFACES 6
typedef struct {
    hid_host_device_handle_t handle;
    bool used, has_ids, is_kbd, is_mouse;
    field_t f[MAX_FIELDS];
    int nf;
    uint8_t buttons;
} iface_t;

static iface_t s_if[MAX_IFACES];
static QueueHandle_t s_evt_queue;
static int s_kbd_count, s_mouse_count;

int usb_keyboards(void) { return s_kbd_count; }
int usb_mice(void) { return s_mouse_count; }

// How many devices the USB host has mounted. A hub counts as one and its keyboard and mouse are not
// visible yet, so this tells an empty port apart from a hub that is still working through what is
// plugged into it. The boot splash uses that to decide whether to keep waiting.
int usb_devices_mounted(void)
{
    usb_host_lib_info_t info = {0};
    return usb_host_lib_info(&info) == ESP_OK ? (int)info.num_devices : 0;
}

// ---------------------------------------------------------------- report descriptor parser

static void add_field(iface_t *it, field_t f)
{
    if (it->nf < MAX_FIELDS) it->f[it->nf++] = f;
}

static void parse_descriptor(iface_t *it, const uint8_t *d, size_t len)
{
    uint16_t usage_page = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t rsize = 0, rcount = 0;
    uint8_t rid = 0;
    uint16_t usages[16];
    int nusages = 0;
    uint16_t umin = 0, umax = 0;
    uint16_t bitpos[8] = {0};       // input bit position per report ID slot
    uint8_t ids[8] = {0};
    int nids = 1, cur = 0;
    it->nf = 0; it->has_ids = false; it->is_kbd = it->is_mouse = false;

    for (size_t i = 0; i < len;) {
        uint8_t b = d[i++];
        if (b == 0xFE) { if (i + 1 < len) i += 2 + d[i]; continue; }  // long item
        int sz = b & 3; if (sz == 3) sz = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        uint32_t v = 0;
        for (int k = 0; k < sz && i + k < len; k++) v |= (uint32_t)d[i + k] << (8 * k);
        int32_t sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
        i += sz;
        if (type == 1) {            // global
            switch (tag) {
            case 0: usage_page = v; break;
            case 1: lmin = sv; break;
            case 2: lmax = sv; break;
            case 7: rsize = v; break;
            case 8:
                rid = v; it->has_ids = true;
                cur = -1;
                for (int k = 0; k < nids; k++) if (ids[k] == rid) cur = k;
                if (cur < 0) { if (nids == 1 && ids[0] == 0 && bitpos[0] == 0) cur = 0; else if (nids < 8) cur = nids++; else cur = 7; ids[cur] = rid; }
                break;
            case 9: rcount = v; break;
            }
        } else if (type == 2) {     // local
            if (tag == 0 && nusages < 16) usages[nusages++] = v;
            else if (tag == 1) umin = v;
            else if (tag == 2) umax = v;
        } else if (type == 0) {     // main
            if (tag == 8) {         // Input
                bool is_const = v & 1, is_var = v & 2, is_rel = v & 4;
                uint16_t pos = bitpos[cur];
                if (!is_const) {
                    if (usage_page == 7) {
                        it->is_kbd = true;
                        if (is_var && rsize == 1) add_field(it, (field_t){F_KEY_MODS, rid, pos, 1, rcount, false, 0, 1, (uint8_t)umin});
                        else if (!is_var && rsize == 8) add_field(it, (field_t){F_KEY_ARRAY, rid, pos, 8, rcount, false, lmin, lmax, 0});
                    } else if (usage_page == 9 && is_var && rsize == 1) {
                        it->is_mouse = true;
                        add_field(it, (field_t){F_BUTTONS, rid, pos, 1, rcount > 8 ? 8 : rcount, false, 0, 1, (uint8_t)(umin ? umin : 1)});
                    } else if (usage_page == 1 && is_var) {
                        for (uint32_t k = 0; k < rcount; k++) {
                            uint16_t u = k < (uint32_t)nusages ? usages[k] : (nusages ? usages[nusages - 1] : (umin + k <= umax ? umin + k : 0));
                            int kind = u == 0x30 ? F_X : u == 0x31 ? F_Y : u == 0x38 ? F_WHEEL : -1;
                            if (kind >= 0) {
                                it->is_mouse = true;
                                add_field(it, (field_t){kind, rid, pos + k * rsize, rsize, 1, is_rel, lmin, lmax, 0});
                            }
                        }
                    }
                }
                bitpos[cur] += rsize * rcount;
            }
            nusages = 0; umin = umax = 0;   // locals reset after every main item
        }
    }
}

static int32_t get_bits(const uint8_t *data, size_t len, int off, int bits, bool is_signed)
{
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        size_t byte = (off + i) >> 3;
        if (byte >= len) break;
        if (data[byte] & (1 << ((off + i) & 7))) v |= 1u << i;
    }
    if (is_signed && bits < 32 && (v & (1u << (bits - 1)))) v |= ~0u << bits;
    return (int32_t)v;
}

static void all_buttons(void)
{
    uint8_t m = 0;
    for (int i = 0; i < MAX_IFACES; i++) if (s_if[i].used) m |= s_if[i].buttons;
    input_mouse_buttons(m);
}

bool usb_debug;

static void handle_report(iface_t *it, const uint8_t *data, size_t len)
{
    if (usb_debug && it->is_kbd) {
        char line[80] = "";
        for (size_t i = 0; i < len && i < 16; i++) sprintf(line + 3 * i, "%02x ", data[i]);
        serial_printf("[usb kbd] %s\n", line);
    }
    uint8_t rid = 0;
    if (it->has_ids) { if (len < 1) return; rid = data[0]; data++; len--; }
    bool kbd = false, mouse_btn = false;
    uint8_t mods = 0, keys[6] = {0};
    int nkeys = 0, dx = 0, dy = 0;
    bool have_abs = false;
    int ax = 0, ay = 0;
    input_mouse_pos(&ax, &ay);
    for (int i = 0; i < it->nf; i++) {
        const field_t *f = &it->f[i];
        if (f->report_id != rid) continue;
        switch (f->kind) {
        case F_KEY_MODS:
            kbd = true;
            for (int k = 0; k < f->count && k < 8; k++) {
                if (get_bits(data, len, f->bit_off + k, 1, false)) mods |= 1 << ((f->usage_min - 0xE0 + k) & 7);
            }
            break;
        case F_KEY_ARRAY:
            kbd = true;
            for (int k = 0; k < f->count; k++) {
                uint8_t u = get_bits(data, len, f->bit_off + k * 8, 8, false);
                if (u >= 1 && nkeys < 6) keys[nkeys++] = u;
            }
            break;
        case F_BUTTONS: {
            uint8_t b = 0;
            for (int k = 0; k < f->count; k++) {
                if (!get_bits(data, len, f->bit_off + k, 1, false)) continue;
                int n = f->usage_min + k;   // 1 = left, 2 = right, 3 = middle
                b |= n == 1 ? BTN_LEFT : n == 2 ? BTN_RIGHT : n == 3 ? BTN_MID : 0;
            }
            it->buttons = b;
            mouse_btn = true;
            break;
        }
        case F_X: case F_Y: {
            int32_t v = get_bits(data, len, f->bit_off, f->bits, f->lmin < 0);
            if (f->relative) { if (f->kind == F_X) dx += v; else dy += v; }
            else {
                int32_t range = f->lmax - f->lmin;
                if (range <= 0) break;
                // An absolute pointer covers what its capture device sees of the picture; on the VGA
                // board that is not the whole scan-out line (display.h: POINTER_SPAN_X).
                int span = f->kind == F_X ? POINTER_SPAN_X : SCREEN_H;
                int p = (int)(((int64_t)(v - f->lmin) * span) / (range + 1));
                if (f->kind == F_X) ax = p - POINTER_OFFS_X; else ay = p;
                have_abs = true;
            }
            break;
        }
        case F_WHEEL: {
            int32_t v = get_bits(data, len, f->bit_off, f->bits, true);
            if (v) input_wheel(v);
            break;
        }
        }
    }
    if (kbd) input_kbd_report(mods, keys, nkeys);
    if (have_abs) input_mouse_abs(ax, ay);
    if (dx || dy) input_mouse_rel(dx, dy);
    if (mouse_btn) all_buttons();
}

// ---------------------------------------------------------------- host glue

typedef struct { hid_host_device_handle_t handle; hid_host_driver_event_t event; } hid_evt_t;

static iface_t *find_iface(hid_host_device_handle_t h)
{
    for (int i = 0; i < MAX_IFACES; i++) if (s_if[i].used && s_if[i].handle == h) return &s_if[i];
    return NULL;
}

static void recount(void)
{
    int k = 0, m = 0;
    for (int i = 0; i < MAX_IFACES; i++) {
        if (!s_if[i].used) continue;
        k += s_if[i].is_kbd; m += s_if[i].is_mouse;
    }
    s_kbd_count = k; s_mouse_count = m;
}

static void iface_cb(hid_host_device_handle_t h, const hid_host_interface_event_t event, void *arg)
{
    iface_t *it = find_iface(h);
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        if (it && hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) == ESP_OK) handle_report(it, data, len);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        hid_host_device_close(h);
        if (it) {
            bool was_kbd = it->is_kbd;
            it->used = false; it->buttons = 0;
            recount();
            if (was_kbd && s_kbd_count == 0) input_release_all();
            all_buttons();
            ESP_LOGI(TAG, "HID interface removed (%d keyboard, %d mouse left)", s_kbd_count, s_mouse_count);
        }
        break;
    default:
        break;
    }
}

static void driver_cb(hid_host_device_handle_t h, const hid_host_driver_event_t event, void *arg)
{
    hid_evt_t e = {.handle = h, .event = event};
    xQueueSend(s_evt_queue, &e, 0);
}

static void open_device(hid_host_device_handle_t h)
{
    iface_t *it = NULL;
    for (int i = 0; i < MAX_IFACES; i++) if (!s_if[i].used) { it = &s_if[i]; break; }
    if (!it) { ESP_LOGW(TAG, "too many HID interfaces"); return; }
    const hid_host_device_config_t cfg = {.callback = iface_cb, .callback_arg = NULL};
    if (hid_host_device_open(h, &cfg) != ESP_OK) return;
    memset(it, 0, sizeof(*it));
    it->handle = h;
    size_t dlen = 0;
    uint8_t *desc = hid_host_get_report_descriptor(h, &dlen);
    if (desc) parse_descriptor(it, desc, dlen);
    hid_host_dev_params_t p;
    hid_host_device_get_params(h, &p);
    if (!it->is_kbd && !it->is_mouse) {
        // nothing we understand (vendor interface): leave it idle
        ESP_LOGI(TAG, "interface %d: no keyboard/mouse usages, ignored", p.iface_num);
        hid_host_device_close(h);
        return;
    }
    it->used = true;
    recount();
    bool abs = false;
    for (int i = 0; i < it->nf; i++) if ((it->f[i].kind == F_X) && !it->f[i].relative) abs = true;
    ESP_LOGI(TAG, "interface %d: %s%s%s, %d fields, report IDs %s", p.iface_num, it->is_kbd ? "keyboard " : "",
             it->is_mouse ? (abs ? "absolute pointer " : "mouse ") : "", "", it->nf, it->has_ids ? "yes" : "no");
    if (hid_host_device_start(h) != ESP_OK) { it->used = false; recount(); }
}

#if CONFIG_IDF_TARGET_ESP32S3
#ifndef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
#error "Stale sdkconfig: delete this build directory's sdkconfig so sdkconfig.defaults.esp32s3 applies"
#endif
// The S3's controller has 8 host channels and every open endpoint keeps one: each device takes one
// for its control endpoint, a hub a second for its status endpoint, each keyboard or mouse interface
// one more. A dock's hubs, Ethernet adapter and billboard device use them up before a receiver's mouse
// gets its turn, so what this OS can never use is turned away, which gives its channel back.
// Only the device descriptor is known at this point. Class 0 means "see the interfaces", which could
// be anything, so those stay unless they are a network adapter recognised by their USB ID.
static bool enum_filter(const usb_device_desc_t *d, uint8_t *config)
{
    (void)config;   // stays at the first configuration
    switch (d->bDeviceClass) {
    case USB_CLASS_HUB:
    case USB_CLASS_HID:
    case USB_CLASS_MISC:
        return true;
    case USB_CLASS_PER_INTERFACE:
        if (d->idVendor == 0x0bda && (d->idProduct & 0xff00) == 0x8100) return false;   // Realtek network
        if (d->idVendor == 0x0b95) return false;                                        // ASIX network
        return true;
    default:
        return false;   // billboard, vendor-specific, and the rest
    }
}
#define USB_ENUM_FILTER enum_filter
#else
#define USB_ENUM_FILTER NULL    // the P4's controller has channels to spare
#endif

static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1,
                                   .enum_filter_cb = USB_ENUM_FILTER};
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    force_full_speed();     // before the first bus reset (the connection is debounced for 500 ms)
    xTaskNotifyGive((TaskHandle_t)arg);
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void hid_task(void *arg)
{
    hid_evt_t e;
    while (1) {
        if (xQueueReceive(s_evt_queue, &e, portMAX_DELAY) && e.event == HID_HOST_DRIVER_EVENT_CONNECTED) open_device(e.handle);
    }
}

// Port watchdog. A device that drops off the bus right after power-up (the JetKVM does: it
// restarts its USB side when it first sees a host) can leave the root port in a failed-reset
// state in which nothing enumerates again. Whenever no device has been attached for a few
// seconds, switch the root port off and on, which restarts connection detection.
volatile int usb_force_cycle;
volatile int usb_wd_off;
volatile int usb_boot_wait;   // set while the boot splash is still waiting for a pointer (boot.c)

static void port_watchdog_task(void *arg)
{
    int idle_s = 0, period = 4;
    for (;;) {
        for (int i = 0; i < 10; i++) { vTaskDelay(pdMS_TO_TICKS(100)); force_full_speed(); }
        if (usb_wd_off) continue;
        // "Idle" = no working keyboard or mouse. That covers an empty port, a port stuck after a
        // failed reset, and a device or hub whose enumeration failed (it still counts as attached).
        if ((s_kbd_count + s_mouse_count) > 0 && !usb_force_cycle) { idle_s = 0; period = 4; continue; }
        // Only an empty port is cycled: with a hub attached, switching the root port off trips an
        // assertion in ESP-IDF's hub driver.
        usb_host_lib_info_t info = {0};
        if (usb_host_lib_info(&info) == ESP_OK && info.num_devices > 0) { idle_s = 0; usb_force_cycle = 0; continue; }
        // While the boot splash is still holding the screen for a pointer, do not back off. A cold
        // wireless receiver raises its connection event whenever it feels like, and if it missed the
        // host's first look at the port the next chance is whatever this wait is: 4 s during boot,
        // 8 and 12 s once the machine is running. Backing off is for a machine that is already up.
        if (usb_boot_wait && period > 4) period = 4;
        if (++idle_s < period && !usb_force_cycle) continue;
        idle_s = 0;
        usb_force_cycle = 0;
        if (period < 12) period += 4;     // back off: a hub with nothing plugged in is left alone longer
        usb_host_lib_set_root_port_power(false);
        vTaskDelay(pdMS_TO_TICKS(400));
        usb_host_lib_set_root_port_power(true);
    }
}

// "usbdev" serial command: every device on the bus, with its interfaces.
static void list_client_cb(const usb_host_client_event_msg_t *msg, void *arg) { }

void usb_list_devices(void)
{
    usb_host_client_handle_t cl;
    const usb_host_client_config_t ccfg = {.is_synchronous = false, .max_num_event_msg = 4,
                                           .async = {.client_event_callback = list_client_cb}};
    if (usb_host_client_register(&ccfg, &cl) != ESP_OK) { serial_printf("usb: no client\n"); return; }
    uint8_t addrs[16];
    int n = 0;
    usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n);
    serial_printf("usb: %d device(s)\n", n);
    for (int i = 0; i < n; i++) {
        usb_device_handle_t dev;
        if (usb_host_device_open(cl, addrs[i], &dev) != ESP_OK) { serial_printf("  addr %d: cannot open\n", addrs[i]); continue; }
        usb_device_info_t info;
        const usb_device_desc_t *dd;
        const usb_config_desc_t *cd;
        usb_host_device_info(dev, &info);
        usb_host_get_device_descriptor(dev, &dd);
        static const char *const speeds[] = {"low", "full", "high"};
        serial_printf("  addr %d: %04x:%04x class %02x, %s speed, %d configuration(s)\n", addrs[i], dd->idVendor, dd->idProduct,
                      dd->bDeviceClass, info.speed < 3 ? speeds[info.speed] : "?", dd->bNumConfigurations);
        if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK) {
            int off = 0;
            const usb_standard_desc_t *d = (const usb_standard_desc_t *)cd;
            while ((d = usb_parse_next_descriptor_of_type(d, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)) != NULL) {
                const usb_intf_desc_t *it = (const usb_intf_desc_t *)d;
                serial_printf("    interface %d: class %02x subclass %02x protocol %02x\n", it->bInterfaceNumber, it->bInterfaceClass,
                              it->bInterfaceSubClass, it->bInterfaceProtocol);
            }
        }
        usb_host_device_close(cl, dev);
    }
    usb_host_client_deregister(cl);
}

void usb_input_start(void)
{
    s_evt_queue = xQueueCreate(10, sizeof(hid_evt_t));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 10, NULL, SYS_CORE);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    const hid_host_driver_config_t hcfg = {
        .create_background_task = true,
        .task_priority = 9,
        .stack_size = 4096,
        .core_id = SYS_CORE,
        .callback = driver_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hcfg));
    xTaskCreatePinnedToCore(hid_task, "hid", 4096, NULL, 8, NULL, SYS_CORE);
    xTaskCreatePinnedToCore(port_watchdog_task, "usb_wd", 3072, NULL, 4, NULL, SYS_CORE);
}
