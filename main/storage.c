// The drive: a wear-levelled FAT filesystem in the internal flash, mounted at /disk.
// A microSD card, when one is in the slot, appears as the folder /disk/SD (a second FAT mount
// inside the first), so Files, the file dialogs and loaded apps reach it without knowing about it.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "board.h"
#include "comp.h"
#include "input.h"
#include "os.h"

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;

static bool s_new_firmware;   // this firmware has not written its bundled files to the drive yet

static void sd_task(void *arg);

static void restore_file(const char *path, const char *data, size_t len, bool if_differs)
{
    struct stat st;
    if (stat(path, &st) == 0 && (!if_differs || ((size_t)st.st_size == len && !s_new_firmware))) return;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

bool storage_init(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = 8,
        .format_if_mount_failed = true,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(DISK_ROOT, "storage", &cfg, &s_wl);
    s_mounted = err == ESP_OK;
    if (!s_mounted) ESP_LOGE("disk", "mount failed: %s", esp_err_to_name(err));
    else {
        // A blank drive (first boot, or reformatted after a power cut damaged it) gets its
        // folders and the welcome file back, so the OS always comes up usable.
        extern const char welcome_start[] asm("_binary_Welcome_txt_start");
        extern const char welcome_end[] asm("_binary_Welcome_txt_end");
        struct stat st;
        // Each firmware build stamps the drive; a different stamp means the bundled programs
        // on the drive may be older than the ones in this firmware.
        char stamp[65] = "", have[65] = "";
        esp_app_get_elf_sha256(stamp, sizeof(stamp));
        FILE *sf = fopen(DISK_ROOT "/System/firmware.id", "rb");
        if (sf) { size_t k = fread(have, 1, 64, sf); have[k] = 0; fclose(sf); }
        s_new_firmware = strcmp(stamp, have) != 0;
        // "blank" = the sample files of this firmware were never installed on this drive
        bool blank = stat(DISK_ROOT "/System/samples-2", &st) != 0;
        mkdir(DISK_ROOT "/Documents", 0777);
        if (blank) {
            restore_file(DISK_ROOT "/Documents/Welcome.txt", welcome_start, welcome_end - welcome_start, false);
        }
        mkdir(DISK_ROOT "/Pictures", 0777);
        mkdir(DISK_ROOT "/Music", 0777);
        mkdir(DISK_ROOT "/Apps", 0777);
        mkdir(DISK_ROOT "/System", 0777);
        mkdir(SD_ROOT, 0777);    // the folder the card is mounted on
        // The bundled programs are kept in the firmware too and put back if missing or outdated.
        #define BUNDLED(sym, file) do { \
            extern const char sym##_start[] asm("_binary_" #sym "_elf_start"); \
            extern const char sym##_end[] asm("_binary_" #sym "_elf_end"); \
            restore_file(DISK_ROOT "/Apps/" file, sym##_start, sym##_end - sym##_start, true); } while (0)
        BUNDLED(Snake, "Snake.elf");
        BUNDLED(Hello, "Hello.elf");
        BUNDLED(CrashTest, "CrashTest.elf");
        BUNDLED(Doom, "Doom.elf");
        extern const char pic_start[] asm("_binary_Bevel_bmp_start");
        extern const char pic_end[] asm("_binary_Bevel_bmp_end");
        if (s_new_firmware) {
            sf = fopen(DISK_ROOT "/System/firmware.id", "wb");
            if (sf) { fwrite(stamp, 1, strlen(stamp), sf); fclose(sf); }
        }
        if (blank) {
            extern const char wav_start[] asm("_binary_Chime_wav_start");
            extern const char wav_end[] asm("_binary_Chime_wav_end");
            restore_file(DISK_ROOT "/Pictures/Bevel.bmp", pic_start, pic_end - pic_start, false);
            restore_file(DISK_ROOT "/Music/Chime.wav", wav_start, wav_end - wav_start, false);
            FILE *m = fopen(DISK_ROOT "/System/samples-2", "wb");
            if (m) fclose(m);
        }
    }
    xTaskCreatePinnedToCore(sd_task, "sdcard", 4096, NULL, 2, NULL, SYS_CORE);
    return s_mounted;
}

bool storage_ok(void) { return s_mounted; }

// ---------------------------------------------------------------- SD card

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_sd_pwr;
static volatile bool s_sd_mounted;

static bool sd_try_mount(void)
{
    if (!s_sd_pwr) {
        // the slot is powered from one of the P4's own regulators
        const sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = BOARD_SD_LDO_CHAN};
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_sd_pwr) != ESP_OK) return false;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;          // slot 1 is the ESP32-C6 (Wi-Fi)
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_sd_pwr;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_SD_CLK; slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0; slot.d1 = BOARD_SD_D1; slot.d2 = BOARD_SD_D2; slot.d3 = BOARD_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_mount_config_t cfg = {.max_files = 6, .format_if_mount_failed = false};
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);   // an empty slot is not an error worth a log line every few seconds
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    return esp_vfs_fat_sdmmc_mount(SD_ROOT, &host, &slot, &cfg, &s_card) == ESP_OK;
}

static void sd_changed(void *arg) { extern void files_refresh_all(void); files_refresh_all(); }

// There is no card-detect switch: look for a card every few seconds, and check that a mounted one
// still answers.
static void sd_task(void *arg)
{
    // The card slot and the ESP32-C6 (Wi-Fi) share the P4's one SDMMC controller. Initialising a
    // card while ESP-Hosted is still bringing up its own slot corrupted memory once during the
    // port (a network frame from the C6 landed in this task's stack), so the first look at the
    // card slot waits until that link is up.
    while (!os_c6_link_up && os_uptime_ms() < 8000) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(300));
    for (;;) {
        if (!s_sd_mounted) {
            if (sd_try_mount()) {
                s_sd_mounted = true;
                ESP_LOGI("disk", "SD card mounted: %s, %llu MB", s_card->cid.name,
                         (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size >> 20);
                ev_call(sd_changed, NULL);
            }
        } else if (sdmmc_get_status(s_card) != ESP_OK) {
            esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card);
            s_card = NULL;
            s_sd_mounted = false;
            ESP_LOGI("disk", "SD card removed");
            ev_call(sd_changed, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(s_sd_mounted ? 2000 : 3000));
    }
}

bool storage_sd_ok(void) { return s_sd_mounted; }

void os_sd_summary(char *buf, int n)
{
    uint64_t t, f;
    if (!s_sd_mounted || esp_vfs_fat_info(SD_ROOT, &t, &f) != ESP_OK) { snprintf(buf, n, "no card"); return; }
    char a[20], b[20];
    os_fmt_size(a, sizeof(a), f);
    os_fmt_size(b, sizeof(b), t);
    snprintf(buf, n, "%s free of %s", a, b);
}

bool storage_df(uint64_t *total, uint64_t *free_bytes)
{
    if (!s_mounted) return false;
    return esp_vfs_fat_info(DISK_ROOT, total, free_bytes) == ESP_OK;
}

bool storage_df_path(const char *path, uint64_t *total, uint64_t *free_bytes)
{
    size_t n = strlen(SD_ROOT);
    if (s_sd_mounted && !strncmp(path, SD_ROOT, n) && (path[n] == '/' || !path[n])) return esp_vfs_fat_info(SD_ROOT, total, free_bytes) == ESP_OK;
    return storage_df(total, free_bytes);
}

void os_shutdown_prepare(void)
{
    extern bool settings_save(void);
    settings_save();
}

void storage_unmount(void)
{
    if (s_sd_mounted) { s_sd_mounted = false; esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card); s_card = NULL; }
    if (!s_mounted) return;
    esp_vfs_fat_spiflash_unmount_rw_wl(DISK_ROOT, s_wl);
    s_mounted = false;
}

void os_fmt_size(char *buf, int n, uint64_t bytes)
{
    if (bytes < 1024) snprintf(buf, n, "%u bytes", (unsigned)bytes);
    else if (bytes < 1024 * 1024) snprintf(buf, n, "%u.%u KB", (unsigned)(bytes / 1024), (unsigned)(bytes % 1024 * 10 / 1024));
    else if (bytes < (1ull << 30)) snprintf(buf, n, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes & 0xFFFFF) * 10 >> 20));
    else snprintf(buf, n, "%u.%u GB", (unsigned)(bytes >> 30), (unsigned)(((bytes >> 20) & 0x3FF) * 10 >> 10));
}

void os_disk_summary(char *buf, int n)
{
    uint64_t t, f;
    if (!storage_df(&t, &f)) { snprintf(buf, n, "not mounted"); return; }
    char a[20], b[20];
    os_fmt_size(a, sizeof(a), f);
    os_fmt_size(b, sizeof(b), t);
    snprintf(buf, n, "%s free of %s (flash)", a, b);
}

void *os_grow(void *old, size_t old_bytes, size_t new_bytes)
{
    void *n = heap_caps_malloc(new_bytes, MALLOC_CAP_SPIRAM);
    if (!n) return NULL;
    if (old) {
        // copy in slices so this task can be preempted and the bus is shared fairly
        for (size_t off = 0; off < old_bytes; off += 16384) {
            size_t k = old_bytes - off < 16384 ? old_bytes - off : 16384;
            memcpy((char *)n + off, (char *)old + off, k);
        }
        free(old);
    }
    return n;
}
