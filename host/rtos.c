// FreeRTOS API on pthreads, for the desktop simulator.
//
// Tasks are threads that really run in parallel (priorities and core affinity are recorded but
// not enforced). Queues and semaphores block on POSIX semaphores only, never on condition
// variables, so a task can be stopped or deleted from the outside (vTaskSuspend/vTaskDelete of
// another task, used by the task list to end a hung app) with a signal at any point where it
// waits, without leaving a lock or a condition variable in a broken state.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include "host_port.h"

#define SIG_CTL SIGUSR2            // suspend / delete requests
#define MIN_STACK (1024 * 1024)    // x86-64 frames are bigger than Xtensa ones

struct host_task {
    pthread_t th;
    char name[24];
    TaskFunction_t fn;
    void *arg;
    int core;
    sigjmp_buf exit_jb;            // vTaskDelete from outside lands here
    volatile int suspend_req, kill_req;
    sem_t resume, ack, dead, notify;
};

static __thread struct host_task *tl_self;

// ---------------------------------------------------------------- timed waits

static void deadline(struct timespec *ts, TickType_t ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

// Waits on s for up to ms (portMAX_DELAY = forever). True if it was taken.
static bool sem_take(sem_t *s, TickType_t ms)
{
    if (ms == 0) return sem_trywait(s) == 0;
    if (ms == portMAX_DELAY) {
        while (sem_wait(s) != 0) if (errno != EINTR) return false;
        return true;
    }
    struct timespec ts;
    deadline(&ts, ms);
    for (;;) {
        if (sem_clockwait(s, CLOCK_MONOTONIC, &ts) == 0) return true;
        if (errno != EINTR) return false;
    }
}

// ---------------------------------------------------------------- tasks

static void ctl_handler(int sig)
{
    (void)sig;
    struct host_task *t = tl_self;
    if (!t) return;
    int saved = errno;
    if (t->suspend_req && !t->kill_req) {
        sem_post(&t->ack);
        while (t->suspend_req && !t->kill_req) sem_wait(&t->resume);
    }
    if (t->kill_req) siglongjmp(t->exit_jb, 1);
    errno = saved;
}

static void ctl_init(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = ctl_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIG_CTL, &sa, NULL);   // no SA_RESTART: blocking calls return EINTR and retry
}

static void install_ctl(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ctl_init);
}

static struct host_task *task_new(const char *name)
{
    struct host_task *t = calloc(1, sizeof(*t));
    strlcpy(t->name, name ? name : "task", sizeof(t->name));
    sem_init(&t->resume, 0, 0);
    sem_init(&t->ack, 0, 0);
    sem_init(&t->dead, 0, 0);
    sem_init(&t->notify, 0, 0);
    return t;
}

static void *trampoline(void *p)
{
    struct host_task *t = p;
    tl_self = t;
    pthread_setname_np(pthread_self(), t->name);
    // an alternate stack, so a stack overflow in an app still reaches the fault handler
    stack_t ss = {.ss_sp = malloc(SIGSTKSZ * 4), .ss_size = SIGSTKSZ * 4};
    sigaltstack(&ss, NULL);
    if (sigsetjmp(t->exit_jb, 1) == 0) t->fn(t->arg);
    sem_post(&t->dead);
    // The task struct is not freed: other tasks may still hold its handle.
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    (void)prio;
    install_ctl();
    struct host_task *t = task_new(name);
    t->fn = fn;
    t->arg = arg;
    t->core = core == tskNO_AFFINITY ? 0 : core;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack * 4 > MIN_STACK ? stack * 4 : MIN_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (out) *out = t;   // before the thread runs: it may look itself up by handle at once
    int err = pthread_create(&t->th, &attr, trampoline, t);
    pthread_attr_destroy(&attr);
    if (err) { if (out) *out = NULL; return pdFAIL; }
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    if (!tl_self) {
        // a thread not created as a task (main, SDL): give it an identity on first use
        tl_self = task_new("main");
        tl_self->th = pthread_self();
    }
    return tl_self;
}

void vTaskDelete(TaskHandle_t t)
{
    struct host_task *self = xTaskGetCurrentTaskHandle();
    if (!t || t == self) {
        if (self->fn) siglongjmp(self->exit_jb, 1);
        pthread_exit(NULL);
    }
    t->kill_req = 1;
    sem_post(&t->resume);
    pthread_kill(t->th, SIG_CTL);
    sem_take(&t->dead, 2000);   // a task that is gone never runs again
}

void vTaskSuspend(TaskHandle_t t)
{
    struct host_task *self = xTaskGetCurrentTaskHandle();
    if (!t || t == self) {
        self->suspend_req = 1;
        while (self->suspend_req && !self->kill_req) sem_wait(&self->resume);
        if (self->kill_req && self->fn) siglongjmp(self->exit_jb, 1);
        return;
    }
    t->suspend_req = 1;
    pthread_kill(t->th, SIG_CTL);
    sem_take(&t->ack, 1000);    // returns once the task has stopped
}

void vTaskResume(TaskHandle_t t)
{
    if (!t || !t->suspend_req) return;
    t->suspend_req = 0;
    sem_post(&t->resume);
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec ts = {ticks / 1000, (long)(ticks % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

void taskYIELD(void) { sched_yield(); }

TickType_t xTaskGetTickCount(void) { return (TickType_t)(esp_timer_get_time() / 1000); }

BaseType_t xTaskNotifyGive(TaskHandle_t t) { sem_post(&t->notify); return pdPASS; }

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    struct host_task *self = xTaskGetCurrentTaskHandle();
    if (!sem_take(&self->notify, wait)) return 0;
    uint32_t n = 1;
    if (clear) while (sem_trywait(&self->notify) == 0) n++;
    else n = 1;
    return n;
}

BaseType_t xPortGetCoreID(void) { return xTaskGetCurrentTaskHandle()->core; }
BaseType_t xPortInIsrContext(void) { return 0; }

// ---------------------------------------------------------------- queues and semaphores
// A semaphore is a queue of zero-sized items: binary = capacity 1, mutex = capacity 1, full.

struct host_queue {
    pthread_mutex_t m;
    sem_t items, spaces;
    uint8_t *buf;
    int len, size, head, count;
};

static struct host_queue *queue_new(int len, int size, int initial)
{
    struct host_queue *q = calloc(1, sizeof(*q));
    pthread_mutex_init(&q->m, NULL);
    sem_init(&q->items, 0, initial);
    sem_init(&q->spaces, 0, len - initial);
    q->len = len;
    q->size = size;
    q->count = initial;
    if (size) q->buf = malloc((size_t)len * size);
    return q;
}

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item) { return queue_new(len, item, 0); }

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    if (!sem_take(&q->spaces, wait)) return pdFAIL;
    pthread_mutex_lock(&q->m);
    if (q->size) memcpy(q->buf + (size_t)((q->head + q->count) % q->len) * q->size, item, q->size);
    q->count++;
    pthread_mutex_unlock(&q->m);
    sem_post(&q->items);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    if (!sem_take(&q->items, wait)) return pdFAIL;
    pthread_mutex_lock(&q->m);
    if (q->size) memcpy(item, q->buf + (size_t)q->head * q->size, q->size);
    q->head = (q->head + 1) % q->len;
    q->count--;
    pthread_mutex_unlock(&q->m);
    sem_post(&q->spaces);
    return pdPASS;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    int n = q->count;
    pthread_mutex_unlock(&q->m);
    return n;
}

void vQueueDelete(QueueHandle_t q)
{
    if (!q) return;
    sem_destroy(&q->items);
    sem_destroy(&q->spaces);
    pthread_mutex_destroy(&q->m);
    free(q->buf);
    free(q);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return queue_new(1, 0, 0); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return queue_new(1, 0, 1); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { return xQueueReceive(s, NULL, wait); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { return xQueueSend(s, NULL, 0); }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{
    if (woken) *woken = pdFALSE;
    return xSemaphoreGive(s);
}
