// Bevel OS: boot and the GUI task.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "os.h"
#include "vga.h"
#include "comp.h"
#include "input.h"
#include "wm.h"
#include "apps.h"
#include "fsutil.h"
#include "net.h"
#include "vnc.h"
#if CONFIG_BEVEL_USB_INPUT
#include "usb/usb_host.h"
#include "soc/usb_dwc_struct.h"
#include "soc/gpio_reg.h"
#include "esp_rom_sys.h"
#endif

static const char *TAG = "os";

void usb_input_start(void);
void settings_load(void);
void app_loader_start(void);

static void run_perf(void *arg) { perf_run(arg); }

// State dump for scripted tests: every window, its flags and focused control.
static void dump_windows(void *arg)
{
    window_t *a = wm_active();
    serial_printf("active: %s | shell focus: %d | menu: %d\n", a ? a->title : "(none)", shell_has_focus(), menu_is_open());
    for (window_t *w = wm_top(); w; w = w->prev) {
        widget_t *f = w->focus;
        serial_printf("win \"%s\" %d,%d %dx%d flags %04x focus %s \"%s\"\n", w->title, w->r.x, w->r.y, w->r.w, w->r.h,
                      w->flags, f ? f->cls->name : "-", f && f->text ? f->text : "");
    }
}

static void launch_cmd(void *arg) { app_launch_name(arg, NULL); }
static void run_cmd(void *arg) { os_run_elf(arg); free(arg); }

bool os_serial_command(const char *cmd, char *arg)
{
    if (!strcmp(cmd, "perf")) { ev_call_sync(run_perf, arg); ev_call_sync(NULL, NULL); return true; }
    if (!strcmp(cmd, "wins")) { ev_call_sync(dump_windows, NULL); return true; }
    if (!strcmp(cmd, "cat")) {
        int n = 0;
        char *t = fs_read_all(arg, &n);
        if (!t) { serial_printf("(no such file)\n"); return true; }
        serial_printf("@@FILE %d\n", n);
        for (int i = 0; i < n; i += 200) serial_printf("%.200s", t + i);
        serial_printf("\n");
        free(t);
        return true;
    }
    if (!strcmp(cmd, "write")) {
        // write <path> <text>: repeats the text to 64 KB, for power-cut tests
        char *sp = strchr(arg, ' ');
        if (!sp) return false;
        *sp++ = 0;
        FILE *f = fopen(arg, "wb");
        if (!f) { serial_printf("failed\n"); return true; }
        int n = strlen(sp);
        for (int i = 0; i < 65536; i += n) fwrite(sp, 1, n, f);
        fclose(f);
        serial_printf("written\n");
        return true;
    }
    if (!strcmp(cmd, "vstat")) {
        // display timing only: touches no heap, so it cannot disturb what it measures
        vga_stats_t v;
        vga_get_stats(&v, true);
        serial_printf("vga: fills %u isr_max %u us slack_min %d us late %u\n", (unsigned)v.fills, (unsigned)v.isr_max_us, (int)v.slack_min_us, (unsigned)v.late);
        return true;
    }
    if (!strcmp(cmd, "net")) {
        net_status_t st;
        net_status(&st);
        serial_printf("net: state %d ssid \"%s\" ip %s rssi %d\n", st.state, st.ssid, st.ip, st.rssi);
        return true;
    }
    if (!strcmp(cmd, "browse")) { ev_call((void (*)(void *))app_browser, strdup(arg)); vTaskDelay(pdMS_TO_TICKS(300)); ev_call_sync(NULL, NULL); return true; }
    if (!strcmp(cmd, "vnc")) { char st[96]; vnc_status(st, sizeof(st)); serial_printf("%s\n", st); return true; }
#if CONFIG_BEVEL_USB_INPUT
    if (!strcmp(cmd, "usbport")) {
        usb_dwc_hprt_reg_t h = USB_DWC.hprt_reg;
        usb_host_lib_info_t info = {0};
        usb_host_lib_info(&info);
        serial_printf("port: power=%d connected=%d enabled=%d D+=%d D-=%d speed=%d devices=%d clients=%d\n", h.prtpwr, h.prtconnsts,
                      h.prtena, h.prtlnsts & 1, (h.prtlnsts >> 1) & 1, h.prtspd, info.num_devices, info.num_clients);
        return true;
    }
    if (!strcmp(cmd, "usbwd")) { extern volatile int usb_wd_off; usb_wd_off = !atoi(arg); return true; }
    if (!strcmp(cmd, "usbpins")) {
        // raw pad levels, independent of the USB controller's own status register
        int hi19 = 0, hi20 = 0;
        for (int i = 0; i < 2000; i++) { hi19 += (REG_READ(GPIO_IN_REG) >> 19) & 1; hi20 += (REG_READ(GPIO_IN_REG) >> 20) & 1; esp_rom_delay_us(500); }
        serial_printf("over 1 s: D- (GPIO19) high %d%%, D+ (GPIO20) high %d%%\n", hi19 / 20, hi20 / 20);
        return true;
    }
    if (!strcmp(cmd, "usbcycle")) { extern volatile int usb_force_cycle; usb_force_cycle = 1; return true; }
    if (!strcmp(cmd, "usbstat")) { extern int usb_keyboards(void), usb_mice(void); serial_printf("keyboards %d mice %d\n", usb_keyboards(), usb_mice()); return true; }
    if (!strcmp(cmd, "usblog")) {
        static const char *const tags[] = {"HUB", "ENUM", "USBH", "USB HOST", "HCD DWC", "EXT_HUB", "EXT_PORT"};
        for (int i = 0; i < 7; i++) esp_log_level_set(tags[i], atoi(arg) ? ESP_LOG_DEBUG : ESP_LOG_INFO);
        return true;
    }
    if (!strcmp(cmd, "usbdbg")) { extern bool usb_debug; usb_debug = atoi(arg); return true; }
#endif
    if (!strcmp(cmd, "sum")) {
        // Adler-32 of a file, to check an upload end to end
        FILE *f = fopen(arg, "rb");
        if (!f) { serial_printf("-1\n"); return true; }
        static uint8_t buf[4096];
        uint32_t s1 = 1, s2 = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            for (size_t i = 0; i < n; i++) { s1 = (s1 + buf[i]) % 65521; s2 = (s2 + s1) % 65521; }
        fclose(f);
        serial_printf("%u\n", (unsigned)((s2 << 16) | s1));
        return true;
    }
    if (!strcmp(cmd, "size")) { struct stat st; serial_printf("%ld\n", stat(arg, &st) == 0 ? (long)st.st_size : -1L); return true; }
    if (!strcmp(cmd, "exists")) { serial_printf("%s\n", fs_is_dir(arg) ? "dir" : fs_exists(arg) ? "file" : "none"); return true; }
    if (!strcmp(cmd, "rm")) { serial_printf("%s\n", fs_delete(arg) ? "deleted" : "failed"); return true; }
    if (!strcmp(cmd, "run")) { ev_call(run_cmd, strdup(arg)); vTaskDelay(pdMS_TO_TICKS(300)); ev_call_sync(NULL, NULL); return true; }
    if (!strcmp(cmd, "launch")) { ev_call(launch_cmd, arg); vTaskDelay(pdMS_TO_TICKS(150)); ev_call_sync(NULL, NULL); return true; }
    return false;
}

static void vga_start(void *arg)
{
    ESP_ERROR_CHECK(vga_init());
    if (arg) { xTaskNotifyGive((TaskHandle_t)arg); vTaskDelete(NULL); }
}

static void gui_task(void *unused)
{
    if (VGA_ISR_CORE == xPortGetCoreID()) vga_start(NULL);
    else {
        // the scan-out interrupt lives on whichever core starts the panel
        xTaskCreatePinnedToCore(vga_start, "vga_init", 4096, xTaskGetCurrentTaskHandle(), 10, NULL, VGA_ISR_CORE);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    storage_init();
    settings_load();
    wm_init();
    app_loader_start();
    net_boot();
    vnc_start();
    vga_cursor_show(true);
    wm_pump(0);
    ESP_LOGI(TAG, "desktop up at %u ms", (unsigned)os_uptime_ms());
    wm_run();
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, OS_NAME " " OS_VERSION ": ESP32-S3 rev v%d.%d, %d cores, PSRAM %u KB, free internal %u KB",
             chip.revision / 100, chip.revision % 100, chip.cores, (unsigned)(esp_psram_get_size() >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    input_init();
    serial_start();
    xTaskCreatePinnedToCore(gui_task, "gui", 8192, NULL, 5, NULL, GUI_CORE);
#if CONFIG_BEVEL_USB_INPUT
    usb_input_start();
#endif
}
