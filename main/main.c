// Bevel OS: boot and the GUI task.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "boot.h"
#include "display.h"
#include "comp.h"
#include "input.h"
#include "wm.h"
#include "apps.h"
#include "fsutil.h"
#include "net.h"
#include "usb/usb_host.h"
#include "audio.h"
#include "esp_event.h"
#if BOARD_HAS_WIFI_REMOTE
#include "esp_hosted_event.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
// The S3's own USB controller, read directly by the "usbport" and "usbpins" debug commands.
#include "soc/usb_dwc_struct.h"
#include "soc/gpio_reg.h"
#include "esp_rom_sys.h"
#endif

static const char *TAG = "os";

// How long the splash holds the last ninth of the bar when there is nothing on the USB bus at all.
// boot.c extends it by a few seconds when something *is* mounted (a hub, or a receiver whose
// keyboard and mouse have not appeared yet) and ends it as soon as a pointer answers, so this number
// is only paid by a machine with nothing attached: raise it if a cold receiver enumerates later.
#define BOOT_INPUT_WAIT_MS 4000

void usb_input_start(void);
#if BOARD_HAS_TOUCH
void touch_start(void);
void touch_inject(int n, const int *xy);
#endif
void settings_load(void);
void app_loader_start(void);
void app_stack_report(void);

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

static void boot_time_report(void *arg)
{
    (void)arg;
    extern int usb_keyboards(void), usb_mice(void), usb_devices_mounted(void);
    int up = boot_elapsed_ms();
    serial_printf("boot: splash gone at %d ms, desktop answering at %u ms (usb: %d keyboard(s), "
                  "%d mouse(es), %d device(s) on the bus)\n", up, (unsigned)boot_ready_ms(),
                  usb_keyboards(), usb_mice(), usb_devices_mounted());
}

bool os_serial_command(const char *cmd, char *arg)
{
    if (!strcmp(cmd, "perf")) { ev_call_sync(run_perf, arg); ev_call_sync(NULL, NULL); return true; }
    if (!strcmp(cmd, "wins")) { ev_call_sync(dump_windows, NULL); return true; }
    if (!strcmp(cmd, "boottime")) { boot_time_report(NULL); return true; }
    if (!strcmp(cmd, "appstack")) { app_stack_report(); return true; }
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
    if (!strcmp(cmd, "net")) {
        net_status_t st;
        net_status(&st);
#if BOARD_HAS_ETH
        serial_printf("net: eth %s %s | wifi state %d ssid \"%s\" ip %s rssi %d\n", st.eth_link ? "up" : "down", st.eth_ip,
                      st.state, st.ssid, st.ip, st.rssi);
#else
        serial_printf("net: wifi state %d ssid \"%s\" ip %s rssi %d\n", st.state, st.ssid, st.ip, st.rssi);
#endif
        return true;
    }
#if DISP_HAS_SCANOUT
    if (!strcmp(cmd, "vstat")) {
        // display timing only: touches no heap, so it cannot disturb what it measures
        disp_scanout_t v;
        disp_get_scanout(&v, true);
        serial_printf("vga: fills %u isr_max %u us slack_min %d us late %u\n", (unsigned)v.fills, (unsigned)v.isr_max_us,
                      (int)v.slack_min_us, (unsigned)v.late);
        return true;
    }
#endif
    if (!strcmp(cmd, "browse")) { ev_call((void (*)(void *))app_browser, strdup(arg)); vTaskDelay(pdMS_TO_TICKS(300)); ev_call_sync(NULL, NULL); return true; }
    if (!strcmp(cmd, "usbport")) {
        usb_host_lib_info_t info = {0};
        usb_host_lib_info(&info);
#if CONFIG_IDF_TARGET_ESP32S3
        usb_dwc_hprt_reg_t h = USB_DWC.hprt_reg;
        serial_printf("port: power=%d connected=%d enabled=%d D+=%d D-=%d speed=%d devices=%d clients=%d\n", h.prtpwr, h.prtconnsts,
                      h.prtena, h.prtlnsts & 1, (h.prtlnsts >> 1) & 1, h.prtspd, info.num_devices, info.num_clients);
#else
        serial_printf("usb: devices=%d clients=%d\n", info.num_devices, info.num_clients);
#endif
        return true;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    if (!strcmp(cmd, "usbpins")) {
        // raw pad levels, independent of the USB controller's own status register
        int hi19 = 0, hi20 = 0;
        for (int i = 0; i < 2000; i++) { hi19 += (REG_READ(GPIO_IN_REG) >> BOARD_PIN_USB_DM) & 1;
                                        hi20 += (REG_READ(GPIO_IN_REG) >> BOARD_PIN_USB_DP) & 1; esp_rom_delay_us(500); }
        serial_printf("over 1 s: D- (GPIO%d) high %d%%, D+ (GPIO%d) high %d%%\n",
                      BOARD_PIN_USB_DM, hi19 / 20, BOARD_PIN_USB_DP, hi20 / 20);
        return true;
    }
#endif
    if (!strcmp(cmd, "usbwd")) { extern volatile int usb_wd_off; usb_wd_off = !atoi(arg); return true; }
    if (!strcmp(cmd, "usbdev")) { extern void usb_list_devices(void); usb_list_devices(); return true; }
#if BOARD_HAS_AUDIO
    if (!strcmp(cmd, "audioout")) {     // audioout [0|1]: show or choose the output
        if (*arg) audio_set_output(atoi(arg));
        vTaskDelay(pdMS_TO_TICKS(200));
        serial_printf("audio output %d: %s\n", audio_output(), audio_output_name(audio_output()));
        return true;
    }
#endif
    if (!strcmp(cmd, "usbeject")) { serial_printf("%s\n", usb_storage_eject() ? "ejected" : "no stick mounted"); return true; }
    if (!strcmp(cmd, "usbdisk")) { char s[64]; os_usb_summary(s, sizeof(s)); serial_printf("usb stick: %s\n", s); return true; }
    if (!strcmp(cmd, "usbcycle")) { extern volatile int usb_force_cycle; usb_force_cycle = 1; return true; }
    if (!strcmp(cmd, "usbstat")) { extern int usb_keyboards(void), usb_mice(void); serial_printf("keyboards %d mice %d\n", usb_keyboards(), usb_mice()); return true; }
    if (!strcmp(cmd, "usblog")) {
        static const char *const tags[] = {"HUB", "ENUM", "USBH", "USB HOST", "HCD DWC", "EXT_HUB", "EXT_PORT"};
        for (int i = 0; i < 7; i++) esp_log_level_set(tags[i], atoi(arg) ? ESP_LOG_DEBUG : ESP_LOG_INFO);
        return true;
    }
    if (!strcmp(cmd, "usbdbg")) { extern bool usb_debug; usb_debug = atoi(arg); return true; }
    if (!strcmp(cmd, "exists")) { serial_printf("%s\n", fs_is_dir(arg) ? "dir" : fs_exists(arg) ? "file" : "none"); return true; }
    if (!strcmp(cmd, "rm")) { serial_printf("%s\n", fs_delete(arg) ? "deleted" : "failed"); return true; }
#if BOARD_HAS_TOUCH
    if (!strcmp(cmd, "touch")) {        // touch [x y [x2 y2]]: fingers as the panel would report them
        int p[4], n = sscanf(arg, "%d %d %d %d", &p[0], &p[1], &p[2], &p[3]);
        touch_inject(n >= 4 ? 2 : n >= 2 ? 1 : 0, p);
        return true;
    }
#endif
    if (!strcmp(cmd, "launch")) { ev_call(launch_cmd, arg); vTaskDelay(pdMS_TO_TICKS(150)); ev_call_sync(NULL, NULL); return true; }
    return false;
}

#if BOARD_HAS_WIFI_REMOTE
volatile bool os_c6_link_up;
static void hosted_up(void *arg, esp_event_base_t base, int32_t id, void *data) { os_c6_link_up = true; }
#endif

static void gui_task(void *unused)
{
    // On the VGA board the scan-out interrupt runs on the core that starts the panel, which is this
    // one: the window manager and the scan-out then share core 1 (docs/PERF.md).
    ESP_ERROR_CHECK(disp_init());
    // From here until boot_done() the splash owns the logical framebuffer: the desktop appears only
    // when everything that answers the mouse is up, and the bar says what is still being started.
    boot_init();
    boot_stage(BOOT_STAGE_DRIVE);
    storage_init();
    boot_sub(BOOT_STAGE_DRIVE, 7, 7, "card slot watching");
    // Sound comes next rather than last: the mixer task spends a while synthesising the system sounds,
    // and starting it here puts that work alongside the drive, the programs and the wait for the pointer
    // instead of after everything - so the startup chime is ready when the desktop appears, not a beat later.
    boot_stage(BOOT_STAGE_SOUND);
    audio_start();
    boot_stage(BOOT_STAGE_SETTINGS);
    settings_load();
    boot_stage(BOOT_STAGE_CLOCK);
    clock_init();
    boot_stage(BOOT_STAGE_DESKTOP);
    wm_init();
    boot_stage(BOOT_STAGE_APPS);
    app_loader_start();
#if BOARD_HAS_TOUCH
    boot_stage(BOOT_STAGE_TOUCH);
    touch_start();
#endif
    boot_stage(BOOT_STAGE_NET);
    net_boot();
    boot_wait_input(BOOT_INPUT_WAIT_MS);   // hold the bar until the pointer answers, see boot.c
    input_pointer_frozen(false);  // the pointer is live from the first desktop frame, centred
    disp_cursor_show(true);
    wm_pump(0);           // the desktop is painted here and takes the screen from the splash
    ESP_LOGI(TAG, "desktop up at %u ms", (unsigned)os_uptime_ms());
    audio_chime(CHIME_STARTUP);
    boot_done();
    wm_run();
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, OS_NAME " " OS_VERSION ": " OS_CHIP " rev v%d.%d, %d cores, PSRAM %u KB, free internal %u KB",
             chip.revision / 100, chip.revision % 100, chip.cores, (unsigned)(esp_psram_get_size() >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    esp_event_loop_create_default();
#if BOARD_HAS_WIFI_REMOTE
    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_UP, hosted_up, NULL);
#endif
    input_init();
    input_pointer_frozen(true);   // pointer input starts with the desktop, not with the splash
    serial_start();
    xTaskCreatePinnedToCore(gui_task, "gui", 8192, NULL, 5, NULL, GUI_CORE);
    usb_input_start();
}
