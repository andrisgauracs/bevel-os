// Bevel OS desktop simulator: the part of the ESP-IDF and FreeRTOS API the OS uses, implemented
// on Linux (pthreads, POSIX). Every ESP-IDF header the OS includes is a one-line file in this
// folder that includes this one, so the OS sources compile unchanged.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- attributes, errors
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define RTC_DATA_ATTR

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_SUPPORTED 0x106
const char *esp_err_to_name(esp_err_t e);
#define ESP_ERROR_CHECK(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { \
    fprintf(stderr, "ESP_ERROR_CHECK failed: %s at %s:%d\n", esp_err_to_name(_e), __FILE__, __LINE__); abort(); } } while (0)

// ---------------------------------------------------------------- log
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
void host_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) host_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) do { } while (0)
static inline void esp_log_level_set(const char *tag, esp_log_level_t l) { (void)tag; (void)l; }

// ---------------------------------------------------------------- memory
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DEFAULT (1 << 12)
void *heap_caps_malloc(size_t n, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps);
void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t esp_psram_get_size(void);

// ---------------------------------------------------------------- system
int64_t esp_timer_get_time(void);
uint32_t esp_cpu_get_cycle_count(void);   // a 240 MHz clock, derived from the monotonic clock
uint32_t esp_random(void);
void esp_restart(void) __attribute__((noreturn));
void esp_rom_delay_us(uint32_t us);
typedef struct { int model; uint32_t features; uint16_t revision; uint8_t cores; } esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *out);
esp_err_t esp_flash_get_size(void *chip, uint32_t *size);
int esp_app_get_elf_sha256(char *dst, size_t size);
typedef void (*esp_ipc_func_t)(void *arg);
esp_err_t esp_ipc_call_blocking(uint32_t cpu, esp_ipc_func_t fn, void *arg);

// ---------------------------------------------------------------- FreeRTOS
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef struct host_task *TaskHandle_t;
typedef struct host_queue *QueueHandle_t;
typedef struct host_queue *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY 0x7FFFFFFF

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, tskNO_AFFINITY)
void vTaskDelete(TaskHandle_t t);
void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t t);
void vTaskResume(TaskHandle_t t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
void taskYIELD(void);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
BaseType_t xPortGetCoreID(void);
BaseType_t xPortInIsrContext(void);

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
#define xQueueSendToBack xQueueSend
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);

SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken);
#define vSemaphoreDelete(s) vQueueDelete(s)

// ---------------------------------------------------------------- UART0 (the serial command channel)
// Served on a TCP port instead (default 47812, BEVEL_SERIAL_PORT), so tools/bv.py can drive the
// simulator like the board: BV_PORT=tcp:127.0.0.1:47812.
#define UART_NUM_0 0
int uart_driver_install(int port, int rx, int tx, int qsize, void *queue, int flags);
int uart_read_bytes(int port, void *buf, uint32_t len, TickType_t wait);
int uart_write_bytes(int port, const void *buf, size_t len);
esp_err_t uart_wait_tx_done(int port, TickType_t wait);

// ---------------------------------------------------------------- drive (a folder on the host)
typedef int32_t wl_handle_t;
#define WL_INVALID_HANDLE -1
typedef struct { bool format_if_mount_failed; int max_files; size_t allocation_unit_size; bool disk_status_check_enable; } esp_vfs_fat_mount_config_t;
esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base, const char *label, const esp_vfs_fat_mount_config_t *cfg, wl_handle_t *wl);
esp_err_t esp_vfs_fat_spiflash_unmount_rw_wl(const char *base, wl_handle_t wl);
esp_err_t esp_vfs_fat_info(const char *base, uint64_t *total, uint64_t *free_bytes);
const char *host_disk_dir(void);    // the folder that appears as /disk

// ---------------------------------------------------------------- misc
int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src, size_t slen);

// GDMA memcpy (benchmark only): not available
typedef void *async_memcpy_handle_t;
typedef struct { int dummy; } async_memcpy_event_t;
typedef struct { int backlog; } async_memcpy_config_t;
typedef bool (*async_memcpy_isr_cb_t)(async_memcpy_handle_t, async_memcpy_event_t *, void *);
#define ASYNC_MEMCPY_DEFAULT_CONFIG() {0}
static inline esp_err_t esp_async_memcpy_install(const async_memcpy_config_t *c, async_memcpy_handle_t *h) { (void)c; (void)h; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t esp_async_memcpy_uninstall(async_memcpy_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t esp_async_memcpy(async_memcpy_handle_t h, void *d, void *s, size_t n, async_memcpy_isr_cb_t cb, void *a)
{ (void)h; (void)d; (void)s; (void)n; (void)cb; (void)a; return ESP_ERR_NOT_SUPPORTED; }

#ifndef strlcpy
size_t strlcpy(char *dst, const char *src, size_t n);
size_t strlcat(char *dst, const char *src, size_t n);
#endif
char *strcasestr(const char *h, const char *n);

#ifdef __cplusplus
}
#endif
