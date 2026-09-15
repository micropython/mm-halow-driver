/*
 * This file is part of mm-halow-driver.
 *
 * Copyright (c) 2026 OpenMV LLC.
 *
 * SPDX-License-Identifier: MIT
 *
 * MMOSAL implementation.
 */
#ifndef MM_HALOW_INCLUDED_HALOW_OSAL_H
#define MM_HALOW_INCLUDED_HALOW_OSAL_H

#include <stdbool.h>
#include <stddef.h>

// Take the driver's memory pool from the host (via mm_halow_port_heap_alloc).
// Must be called from a context where the GC may run, i.e. not from the
// scheduler.  Returns false if the pool could not be allocated.
bool mm_halow_osal_init(void);

// Allocate from the driver's private heap.  morselib runs from interrupt
// context, where the host's GC heap (if any) must not be touched.
void *mm_halow_osal_malloc(size_t size);
void mm_halow_osal_free(void *ptr);

// Run any morselib timers that have expired.  Called from mm_halow_poll().
void mm_halow_osal_timer_poll(void);

// Release the memory pool back to the host.  Only safe once
// morselib has been shut down and the scheduler torn down.
void mm_halow_osal_deinit(void);

// True while morselib holds a critical section, i.e. while interrupts are off
// on its behalf.  Nothing may yield in that window.
bool mm_halow_osal_in_critical(void);

#endif // MM_HALOW_INCLUDED_HALOW_OSAL_H

// Fatal-failure record for post-mortem reads over a debug probe: the console
// often dies with the fault, so the last failure is parked in RAM too.
#define MM_HALOW_FATAL_MAGIC (0x48464154)   // "HFAT"
struct mm_halow_fatal_record {
    uint32_t magic;
    uint32_t pc;
    uint32_t lr;
    uint32_t fileid;
    uint32_t line;
    uint32_t ticks_ms;
};
extern struct mm_halow_fatal_record mm_halow_fatal_record;
