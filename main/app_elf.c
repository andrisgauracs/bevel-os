// App images on the ESP32-S3: position-independent Xtensa ELF files relocated by Espressif's
// elf_loader. A fault inside an app's own code is caught by the per-core exception table and the
// task is sent to app_crash_landing() instead of panicking the whole system.
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_elf.h"
#include "private/elf_symbol.h"
#include "esp_ipc.h"
#include "xtensa_api.h"
#include "xtensa_context.h"
#include "app_plat.h"

#define APP_MAGIC 0x42564C31  // "BVL1", checked by the app's entry shim (apps/crt.c)

void *app_plat_load(const char *path, const uint8_t *img, int len)
{
    (void)path;
    esp_elf_t *elf = calloc(1, sizeof(*elf));
    if (!elf) return NULL;
    esp_elf_init(elf);
    if (esp_elf_relocate(elf, img, len) != 0) { esp_elf_deinit(elf); free(elf); return NULL; }
    return elf;
}

void app_plat_unload(void *image)
{
    esp_elf_deinit(image);
    free(image);
}

void app_plat_run(void *image, const bevel_api_t *api, const char *arg)
{
    char *argv[2] = {(char *)api, (char *)arg};
    esp_elf_request(image, 0, APP_MAGIC, argv);
}

static void app_exception(XtExcFrame *frame)
{
    extern void xt_unhandled_exception(XtExcFrame *frame);
    if (xPortInIsrContext() || !app_fault(frame->exccause, frame->pc, frame->excvaddr)) {
        xt_unhandled_exception(frame);
        return;
    }
    // The faulting app task resumes in app_crash_landing() instead of at the bad instruction.
    frame->pc = (uint32_t)app_crash_landing;
    frame->ps &= ~(3u << 16);       // CALLINC = 0, so the landing function's ENTRY does not rotate the window
    frame->lcount = 0;
}

static void install_handlers(void *arg)
{
    // the exception table is per core
    static const int causes[] = {0, 2, 3, 6, 9, 20, 28, 29};
    for (int i = 0; i < (int)(sizeof(causes) / sizeof(causes[0])); i++) xt_set_exception_handler(causes[i], app_exception);
}

static uintptr_t resolver(const char *name) { return app_libc_symbol(name); }

void app_plat_init(void)
{
    install_handlers(NULL);
    elf_set_symbol_resolver(resolver);
    esp_ipc_call_blocking(1 - xPortGetCoreID(), install_handlers, NULL);
}
