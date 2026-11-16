// ESP32-S3 PIE (SIMD) kernels: 16 bytes per instruction.
// All pointers must be 16-byte aligned; counts are in 16-byte blocks.
#include "sdkconfig.h"
#if CONFIG_BEVEL_PIE

    .text
    .align 4

// void pie_copy16(void *dst, const void *src, int blocks)
    .section .iram1, "ax"
    .global pie_copy16
    .type pie_copy16, @function
    .align 4
pie_copy16:
    entry   a1, 32
    loopnez a4, 1f
    ee.vld.128.ip q0, a3, 16
    ee.vst.128.ip q0, a2, 16
1:  retw

// void pie_fill16(void *dst, int byte, int blocks)
    .global pie_fill16
    .type pie_fill16, @function
    .align 4
pie_fill16:
    entry   a1, 32
    s8i     a3, a1, 0
    ee.vldbc.8 q0, a1
    loopnez a4, 1f
    ee.vst.128.ip q0, a2, 16
1:  retw

// void pie_copy16_isr(void *dst, const void *src, int blocks)
// Same as pie_copy16 but usable from an interrupt handler: it turns the PIE coprocessor on
// by hand and preserves q0, so the lazy coprocessor context of the interrupted task survives.
    .global pie_copy16_isr
    .type pie_copy16_isr, @function
    .align 4
pie_copy16_isr:
    entry   a1, 48
    rsr     a5, cpenable
    movi    a6, 8                   // CP3 = PIE ("cop_ai")
    or      a6, a5, a6
    wsr     a6, cpenable
    rsync
    ee.vst.128.ip q0, a1, 0         // save q0 in our stack frame (a1 is 16-byte aligned)
    loopnez a4, 1f
    ee.vld.128.ip q0, a3, 16
    ee.vst.128.ip q0, a2, 16
1:  ee.vld.128.ip q0, a1, 0
    wsr     a5, cpenable
    rsync
    retw
#endif // CONFIG_BEVEL_PIE
