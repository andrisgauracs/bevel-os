// App images in the desktop simulator: the same apps/*.c built as x86-64 shared objects (still
// named .elf on the drive) and loaded with dlopen.
//
// On the board the loader resolves an app's C library imports to the OS's wrappers (memory and
// files tracked per app, exit() ends the app). Here the same table (app_libc_symbol) is applied
// by rewriting the app's GOT entries after dlopen, so apps behave the same way.
// Faults (SIGSEGV, SIGFPE, SIGILL, SIGBUS) inside an app task are caught and the task is sent to
// app_crash_landing(), like the Xtensa exception handler does on the board.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include "host_port.h"
#include "app_plat.h"

#define APP_MAGIC 0x42564C31

typedef struct { void *dl; int (*entry)(int, char **); } image_t;

static __thread sigjmp_buf *tl_crash_jb;   // set while an app's code runs in this task

static void patch_got(void *dl)
{
    struct link_map *lm;
    if (dlinfo(dl, RTLD_DI_LINKMAP, &lm) != 0) return;
    ElfW(Addr) base = lm->l_addr;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    const ElfW(Rela) *rel[2] = {NULL, NULL};
    size_t relsz[2] = {0, 0};
    for (const ElfW(Dyn) *d = lm->l_ld; d->d_tag != DT_NULL; d++) {
        // glibc relocates these in place; an unrelocated value is below the load address
        ElfW(Addr) p = d->d_un.d_ptr < base ? d->d_un.d_ptr + base : d->d_un.d_ptr;
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = (const ElfW(Sym) *)p; break;
        case DT_STRTAB: strtab = (const char *)p; break;
        case DT_JMPREL: rel[0] = (const ElfW(Rela) *)p; break;
        case DT_PLTRELSZ: relsz[0] = d->d_un.d_val; break;
        case DT_RELA: rel[1] = (const ElfW(Rela) *)p; break;
        case DT_RELASZ: relsz[1] = d->d_un.d_val; break;
        }
    }
    if (!symtab || !strtab) return;
    long page = sysconf(_SC_PAGESIZE);
    for (int t = 0; t < 2; t++) {
        for (size_t i = 0; rel[t] && i < relsz[t] / sizeof(ElfW(Rela)); i++) {
            const ElfW(Rela) *r = &rel[t][i];
            int type = ELF64_R_TYPE(r->r_info);
            if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) continue;
            const char *name = strtab + symtab[ELF64_R_SYM(r->r_info)].st_name;
            uintptr_t fn = app_libc_symbol(name);
            if (!fn) continue;
            void **slot = (void **)(base + r->r_offset);
            uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)(page - 1);
            mprotect((void *)pg, page, PROT_READ | PROT_WRITE);   // RELRO makes the GOT read-only
            *slot = (void *)fn;
        }
    }
}

void *app_plat_load(const char *path, const uint8_t *img, int len)
{
    (void)img; (void)len;
    char host[1200];
    extern const char *host_path(const char *, char *, int);
    void *dl = dlopen(host_path(path, host, sizeof(host)), RTLD_NOW | RTLD_LOCAL);
    if (!dl) { ESP_LOGW("apps", "%s", dlerror()); return NULL; }
    image_t *im = calloc(1, sizeof(*im));
    im->dl = dl;
    im->entry = (int (*)(int, char **))dlsym(dl, "bevel_entry");
    if (!im->entry) { ESP_LOGW("apps", "%s: no bevel_entry (built for the board?)", path); dlclose(dl); free(im); return NULL; }
    patch_got(dl);
    return im;
}

void app_plat_unload(void *image)
{
    image_t *im = image;
    dlclose(im->dl);
    free(im);
}

void app_plat_run(void *image, const bevel_api_t *api, const char *arg)
{
    image_t *im = image;
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1)) {
        tl_crash_jb = NULL;
        app_crash_landing();
    }
    tl_crash_jb = &jb;
    char *argv[2] = {(char *)api, (char *)arg};
    im->entry(APP_MAGIC, argv);
    tl_crash_jb = NULL;
}

static void fault(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uint32_t pc = (uint32_t)uc->uc_mcontext.gregs[REG_RIP];
    // causes as numbered by the Xtensa exception table, which the crash dialog explains
    uint32_t cause = 0;
    if (sig == SIGFPE) cause = 6;
    else if (sig == SIGBUS) cause = 9;
    else if (sig == SIGSEGV) cause = (uc->uc_mcontext.gregs[REG_ERR] & 2) ? 29 : 28;
    if (tl_crash_jb && app_fault(cause, pc, (uint32_t)(uintptr_t)si->si_addr)) siglongjmp(*tl_crash_jb, 1);
    // a fault in the OS itself: crash for real, so a debugger or core dump shows it
    signal(sig, SIG_DFL);
}

void app_plat_init(void)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
