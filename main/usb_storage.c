// USB sticks: a FAT-formatted mass storage device on the USB host port appears as the folder /disk/USB
// (a FAT mount inside the drive, like the SD card), so Files, the file dialogs and loaded apps reach it
// without knowing about it. One stick at a time.
//
// A stick holds three of the controller's host channels, and the S3 only has eight (usb_input.c), so
// the keyboard and mouse come first: the stick is only claimed once the bus has gone quiet, is left
// alone if there is no room, and gives its channels back if an input device turns up later and cannot
// get one. After that, or after "eject", it stays unmounted until it is plugged in again.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_blockdev.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "wm.h"
#include "os.h"

static const char *TAG = "usbdisk";

// The driver keeps one DMA buffer, grown to the largest transfer so far, and if growing it fails it
// is left holding a freed pointer. FatFs asks for whole files in one read when it can. So every
// transfer is cut to this size here, and the buffer is grown to it once, up front (attach()).
#define CHUNK_BYTES   8192
#define SETTLE_MS     1500      // no new device and no HID activity for this long: input has had its turn
#define SETTLE_MAX_MS 10000

#if CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM
#define USB_BUF_CAPS (MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM)
#else
#define USB_BUF_CAPS (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
#endif

typedef enum { EV_ATTACHED, EV_GONE, EV_EJECT } ev_kind_t;
typedef struct { ev_kind_t kind; uint8_t addr; msc_host_device_handle_t dev; } ev_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_io_lock, s_eject_done;

static msc_host_device_handle_t s_dev;
static esp_blockdev_handle_t s_inner;   // the driver's block device
static esp_blockdev_t s_outer;          // what FatFs is given: s_inner, in chunks
static bool s_alive;                    // s_inner may be used (under s_io_lock)
static volatile bool s_mounted;
static const char *volatile s_status = "no stick";

// ---------------------------------------------------------------- block device, in chunks

static esp_err_t chunked(bool write, uint8_t *buf, uint64_t addr, size_t len)
{
    size_t block = s_outer.geometry.read_size;
    size_t max = CHUNK_BYTES - CHUNK_BYTES % block;
    while (len) {
        size_t n = len < max ? len : max;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        xSemaphoreTake(s_io_lock, portMAX_DELAY);
        if (s_alive) err = write ? s_inner->ops->write(s_inner, buf, addr, n)
                                 : s_inner->ops->read(s_inner, buf, n, addr, n);
        xSemaphoreGive(s_io_lock);
        if (err != ESP_OK) return err;
        buf += n; addr += n; len -= n;
    }
    return ESP_OK;
}

static esp_err_t outer_read(esp_blockdev_handle_t h, uint8_t *dst, size_t dst_size, uint64_t addr, size_t len)
{
    if (len > dst_size) return ESP_ERR_INVALID_SIZE;
    return chunked(false, dst, addr, len);
}

static esp_err_t outer_write(esp_blockdev_handle_t h, const uint8_t *src, uint64_t addr, size_t len)
{
    return chunked(true, (uint8_t *)src, addr, len);
}

static const esp_blockdev_ops_t s_outer_ops = {.read = outer_read, .write = outer_write};

// ---------------------------------------------------------------- attach and detach

static void changed(void *arg) { extern void files_refresh_all(void); files_refresh_all(); }

// Grow the driver's buffer to its working size while a failure can still be handled.
static bool prime_buffer(void)
{
    size_t max = CHUNK_BYTES - CHUNK_BYTES % s_inner->geometry.read_size;
    if (heap_caps_get_largest_free_block(USB_BUF_CAPS) < max + 1024) return false;
    uint8_t *tmp = heap_caps_malloc(max, MALLOC_CAP_SPIRAM);
    if (!tmp) return false;
    bool ok = s_inner->ops->read(s_inner, tmp, max, 0, max) == ESP_OK;
    free(tmp);
    return ok;
}

static void release_device(void)
{
    if (s_inner) { msc_host_release_blockdev(s_inner); s_inner = NULL; }
    if (s_dev) { msc_host_uninstall_device(s_dev); s_dev = NULL; }
}

static void attach(uint8_t addr)
{
    esp_err_t err = msc_host_install_device(addr, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        if (err == ESP_ERR_NOT_SUPPORTED) {
            s_status = "not mounted, USB channels in use";
            ESP_LOGW(TAG, "stick left unmounted: no free USB host channels (keyboard and mouse come first)");
        } else {
            s_status = "stick not responding";
            ESP_LOGW(TAG, "stick did not start: %s", esp_err_to_name(err));
        }
        return;
    }
    msc_host_device_info_t info;
    msc_host_get_device_info(s_dev, &info);
    uint64_t bytes = (uint64_t)info.sector_count * info.sector_size;
    ESP_LOGI(TAG, "stick %04x:%04x, %llu MB, %u-byte sectors", info.idVendor, info.idProduct, bytes >> 20, (unsigned)info.sector_size);

    if (msc_host_get_blockdev(s_dev, &s_inner) != ESP_OK || !prime_buffer()) {
        s_status = "stick not usable";
        ESP_LOGW(TAG, "stick: could not read it, or no memory for its buffer");
        release_device();
        return;
    }
    s_outer = *s_inner;
    s_outer.ctx = NULL;
    s_outer.ops = &s_outer_ops;
    s_alive = true;
    const esp_vfs_fat_mount_config_t cfg = {.max_files = 6, .format_if_mount_failed = false};
    err = esp_vfs_fat_bdl_mount(USB_ROOT, &s_outer, &cfg);
    if (err != ESP_OK) {
        s_alive = false;
        s_status = "not FAT formatted";
        ESP_LOGW(TAG, "stick has no FAT file system: %s", esp_err_to_name(err));
        release_device();
        return;
    }
    // The first free-space query can walk the whole FAT: do it here, not in the GUI task's repaint.
    uint64_t total, free_bytes;
    esp_vfs_fat_info(USB_ROOT, &total, &free_bytes);
    s_mounted = true;
    s_status = "";
    ESP_LOGI(TAG, "mounted at " USB_ROOT);
    ev_call(changed, NULL);
}

static void detach(bool still_there, const char *status)
{
    if (s_mounted) {
        s_mounted = false;
        if (!still_there) {     // anything still in flight fails at once instead of waiting for a dead device
            xSemaphoreTake(s_io_lock, portMAX_DELAY);
            s_alive = false;
            xSemaphoreGive(s_io_lock);
        }
        esp_vfs_fat_bdl_unmount(USB_ROOT, &s_outer);
        xSemaphoreTake(s_io_lock, portMAX_DELAY);
        s_alive = false;
        xSemaphoreGive(s_io_lock);
        ESP_LOGI(TAG, "unmounted");
        ev_call(changed, NULL);
    }
    release_device();
    s_status = status;
}

// ---------------------------------------------------------------- task

static void msc_event(const msc_host_event_t *e, void *arg)
{
    ev_t ev = {0};
    if (e->event == MSC_DEVICE_CONNECTED) { ev.kind = EV_ATTACHED; ev.addr = e->device.address; }
    else if (e->event == MSC_DEVICE_DISCONNECTED) { ev.kind = EV_GONE; ev.dev = e->device.handle; }
    else return;
    xQueueSend(s_q, &ev, pdMS_TO_TICKS(100));
}

static void storage_task(void *arg)
{
    extern int usb_devices_mounted(void), usb_hid_activity(void);
    uint8_t pending = 0;
    int quiet = 0, waited = 0, last_dev = -1, last_hid = -1;
    for (;;) {
        ev_t e;
        if (xQueueReceive(s_q, &e, pending ? pdMS_TO_TICKS(100) : portMAX_DELAY)) {
            switch (e.kind) {
            case EV_ATTACHED:
                if (s_dev || pending) { ESP_LOGW(TAG, "one stick at a time: this one is ignored"); break; }
                pending = e.addr;
                quiet = waited = 0; last_dev = last_hid = -1;
                s_status = "starting";
                break;
            case EV_GONE:
                if (e.dev == s_dev) detach(false, "no stick");
                break;
            case EV_EJECT:
                detach(true, "ejected, plug it in again to use it");
                xSemaphoreGive(s_eject_done);
                break;
            }
            continue;
        }
        if (!pending) continue;
        int d = usb_devices_mounted(), h = usb_hid_activity();
        quiet = d == last_dev && h == last_hid ? quiet + 100 : 0;
        last_dev = d; last_hid = h;
        waited += 100;
        if (quiet >= SETTLE_MS || waited >= SETTLE_MAX_MS) {
            attach(pending);
            pending = 0;
        }
    }
}

void usb_storage_start(void)
{
    s_q = xQueueCreate(6, sizeof(ev_t));
    s_io_lock = xSemaphoreCreateMutex();
    s_eject_done = xSemaphoreCreateBinary();
    // The driver's task also completes every transfer, so file access waits on it: it runs above the
    // tasks that read and write files.
    const msc_host_driver_config_t cfg = {
        .create_backround_task = true,
        .task_priority = 7,
        .stack_size = 4096,
        .core_id = SYS_CORE,
        .callback = msc_event,
    };
    ESP_ERROR_CHECK(msc_host_install(&cfg));
    xTaskCreatePinnedToCore(storage_task, "usbdisk", 4096, NULL, 3, NULL, SYS_CORE);
}

// Unmounts the stick and gives back its channels. False if none was mounted.
bool usb_storage_eject(void)
{
    if (!s_mounted) return false;
    xSemaphoreTake(s_eject_done, 0);
    const ev_t e = {.kind = EV_EJECT};
    if (xQueueSend(s_q, &e, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    return xSemaphoreTake(s_eject_done, pdMS_TO_TICKS(8000)) == pdTRUE;
}

bool storage_usb_ok(void) { return s_mounted; }

void os_usb_summary(char *buf, int n)
{
    uint64_t t, f;
    if (!s_mounted || esp_vfs_fat_info(USB_ROOT, &t, &f) != ESP_OK) { snprintf(buf, n, "%s", s_status); return; }
    char a[20], b[20];
    os_fmt_size(a, sizeof(a), f);
    os_fmt_size(b, sizeof(b), t);
    snprintf(buf, n, "%s free of %s", a, b);
}
