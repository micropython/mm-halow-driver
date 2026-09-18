/*
 * This file is part of mm-halow-driver.
 *
 * Copyright (c) 2026 OpenMV LLC.
 *
 * SPDX-License-Identifier: MIT
 *
 * Cooperative task scheduler for the Morse Micro WLAN stack.
 */
#include "mm_halow_config.h"

#if MM_HALOW_ENABLED

#include <string.h>

#include "mm_halow.h"
#include "mm_halow_osal.h"
#include "mm_halow_sched.h"

// Number of morselib tasks that can exist at once.  morselib itself creates two
// (the driver task and the UMAC event loop), plus one more for the SDIO/SPI IRQ
// task when the SDIO transport is in use.
#ifndef MM_HALOW_SCHED_MAX_TASKS
#define MM_HALOW_SCHED_MAX_TASKS  (4)
#endif

// Size of the context saved by mm_halow_context_switch(), in words: r3-r11 and lr,
// plus the callee-saved half of the FPU register file when hardware floating
// point is in use.  Kept even so that stacks stay 8-byte aligned.
#if defined(__ARM_FP)
#define MM_HALOW_CONTEXT_WORDS (10 + 16)
#else
#define MM_HALOW_CONTEXT_WORDS (10)
#endif

static mm_halow_task_t mm_halow_tasks[MM_HALOW_SCHED_MAX_TASKS];

// In creation order, which is the order they run in.
static mm_halow_task_t *mm_halow_task_list;

// The task currently running, or NULL when running the scheduler itself.
static mm_halow_task_t *mm_halow_task_cur;

// Wall clock one mm_halow_sched_run() may spend.  The scheduler is cooperative, so
// a task that stops yielding cannot be preempted -- only denied another turn.
#ifndef MM_HALOW_SCHED_BUDGET_MS
#define MM_HALOW_SCHED_BUDGET_MS (20)
#endif

// Task turns per mm_halow_sched_run() call.
// Ceiling on any single wait, however long the caller asked for.
#ifndef MM_HALOW_SCHED_WAIT_MAX_MS
#define MM_HALOW_SCHED_WAIT_MAX_MS (10000)
#endif

#ifndef MM_HALOW_SCHED_PASSES
#define MM_HALOW_SCHED_PASSES (4)
#endif

// Move the Armv8-M main-stack limit (MSPLIM) to each task's stack. A port whose
// startup points MSPLIM at the main stack needs this: the scheduler runs tasks
// on stacks below that limit, so a push would otherwise fault.
#ifndef MM_HALOW_SCHED_SET_MSPLIM
#define MM_HALOW_SCHED_SET_MSPLIM (0)
#endif
#if MM_HALOW_SCHED_SET_MSPLIM
#if !(defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__))
#error "MM_HALOW_SCHED_SET_MSPLIM requires an Armv8-M main-profile core"
#endif
static inline uint32_t mm_halow_get_msplim(void) {
    uint32_t v;
    __asm volatile ("mrs %0, msplim" : "=r" (v));
    return v;
}
static inline void mm_halow_set_msplim(uint32_t v) {
    __asm volatile ("msr msplim, %0" : : "r" (v) : "memory");
}
#endif

// Stack pointer of whoever called mm_halow_sched_run(), saved while a task runs.
static void *mm_halow_sched_sp;

// Set while mm_halow_sched_run() is walking the task list, to make it re-entrant.
static volatile bool mm_halow_sched_running;

// Switch from the context described by *from_sp to the one at to_sp.  Both
// contexts are cooperative, so only the callee-saved registers need to be
// preserved; the compiler has already spilled anything else it cares about.
static void __attribute__((naked, noinline)) mm_halow_context_switch(void **from_sp, void *to_sp) {
    __asm volatile (
        "push   {r3-r11, lr}    \n"
        #if defined(__ARM_FP)
        "vpush  {d8-d15}        \n"
        #endif
        "str    sp, [r0]        \n"
        "mov    sp, r1          \n"
        #if defined(__ARM_FP)
        "vpop   {d8-d15}        \n"
        #endif
        "pop    {r3-r11, pc}    \n"
        );
}

// True when running in an exception handler, where the MicroPython event loop
// must not be pumped.
// mm_halow_in_irq() comes from the configport: on Cortex-M it reads IPSR.

// Entry trampoline: runs the task main function and then retires the task.
static void mm_halow_task_trampoline(void) {
    mm_halow_task_t *task = mm_halow_task_cur;
    task->entry(task->arg);
    mm_halow_sched_task_delete(NULL);
}

mm_halow_task_t *mm_halow_sched_task_create(void (*entry)(void *), void *arg, size_t stack_words, const char *name) {
    mm_halow_task_t *task = NULL;
    for (size_t i = 0; i < MM_HALOW_SCHED_MAX_TASKS; i++) {
        if (mm_halow_tasks[i].stack == NULL) {
            task = &mm_halow_tasks[i];
            break;
        }
    }
    if (task == NULL) {
        return NULL;
    }

    // Round the stack up to an even number of words so that it stays 8-byte
    // aligned, and reserve room for the initial context.
    stack_words = (stack_words + 1) & ~(size_t)1;
    if (stack_words < MM_HALOW_CONTEXT_WORDS) {
        stack_words = MM_HALOW_CONTEXT_WORDS;
    }
    uint32_t *stack = mm_halow_osal_malloc(stack_words * sizeof(uint32_t));
    if (stack == NULL) {
        return NULL;
    }

    // Paint the stack so that overflow can be detected after the fact via
    // mm_halow_sched_stack_free_words().  The stack sizes come from morselib and
    // are chosen for a FreeRTOS port, not for this scheduler.
    for (size_t i = 0; i < stack_words; i++) {
        stack[i] = MM_HALOW_STACK_FILL;
    }

    memset(task, 0, sizeof(*task));
    task->stack = stack;
    task->stack_words = stack_words;
    task->name = name;
    task->entry = entry;
    task->arg = arg;
    task->state = MM_HALOW_TASK_READY;

    // Build a context that mm_halow_context_switch() can restore, with the
    // trampoline in the slot it pops into pc.
    uint32_t *sp = (uint32_t *)task->stack + stack_words - MM_HALOW_CONTEXT_WORDS;
    memset(sp, 0, MM_HALOW_CONTEXT_WORDS * sizeof(uint32_t));
    sp[MM_HALOW_CONTEXT_WORDS - 1] = (uint32_t)mm_halow_task_trampoline;
    task->sp = sp;

    // Append to the task list, so that tasks run in creation order.
    mm_halow_task_t **tail = &mm_halow_task_list;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = task;

    return task;
}

size_t mm_halow_sched_stack_free_words(const mm_halow_task_t *task) {
    if (task == NULL || task->stack == NULL) {
        return 0;
    }
    const uint32_t *stack = (const uint32_t *)task->stack;
    size_t free_words = 0;
    while (free_words < task->stack_words && stack[free_words] == MM_HALOW_STACK_FILL) {
        free_words++;
    }
    return free_words;
}

void mm_halow_sched_task_delete(mm_halow_task_t *task) {
    if (task == NULL) {
        task = mm_halow_task_cur;
        if (task == NULL) {
            return;
        }
        // Retire the calling task.  Its stack must not be touched again, so
        // switch away without saving anything of interest.
        task->state = MM_HALOW_TASK_DEAD;
        void *discard;
        mm_halow_context_switch(&discard, mm_halow_sched_sp);
        // Unreachable: a dead task is never resumed.
        return;
    }
    task->state = MM_HALOW_TASK_DEAD;
}

mm_halow_task_t *mm_halow_sched_task_current(void) {
    return mm_halow_task_cur;
}

// Held while the transceiver is being serviced, and by which context.  See
// mm_halow_sched_claim() in the header.
#define MM_HALOW_OWNER_NONE   (0)
#define MM_HALOW_OWNER_THREAD (1)
#define MM_HALOW_OWNER_IRQ    (2)

static volatile uint8_t mm_halow_service_owner;
static volatile uint8_t mm_halow_service_depth;

// When the running pass has to be over.  Only meaningful inside
// mm_halow_sched_run(); nothing checks it otherwise.
static uint32_t mm_halow_sched_deadline;

// Whether the pass now running is out of time.  Only meaningful inside
// mm_halow_sched_run(): outside one the deadline is whatever the last pass left
// behind, which is always in the past.
static bool mm_halow_pass_expired(void) {
    return mm_halow_sched_running &&
           (int32_t)(mm_halow_ticks_ms() - mm_halow_sched_deadline) >= 0;
}

bool mm_halow_sched_over_budget(void) {
    // Answered for the running task only.  A bus operation outside a task is
    // the boot firmware download, driven straight from MicroPython with nothing
    // waiting on it -- yielding there would leave the transfer half done.
    return mm_halow_task_cur != NULL && mm_halow_pass_expired();
}

bool mm_halow_sched_in_callback;

bool mm_halow_sched_claim(void) {
    uint8_t owner = mm_halow_in_irq() ? MM_HALOW_OWNER_IRQ : MM_HALOW_OWNER_THREAD;
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool claimed = mm_halow_service_owner == MM_HALOW_OWNER_NONE || mm_halow_service_owner == owner;
    if (claimed) {
        mm_halow_service_owner = owner;
        mm_halow_service_depth++;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return claimed;
}

void mm_halow_sched_release(void) {
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    if (mm_halow_service_depth > 0 && --mm_halow_service_depth == 0) {
        mm_halow_service_owner = MM_HALOW_OWNER_NONE;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
}

void mm_halow_sched_yield(void) {
    mm_halow_task_t *task = mm_halow_task_cur;
    if (task != NULL) {
        mm_halow_context_switch(&task->sp, mm_halow_sched_sp);
    } else {
        // Not task context: run the tasks, holding the poll off for the pass so
        // it cannot cut in on a transfer that is already under way.
        if (mm_halow_sched_claim()) {
            mm_halow_sched_run();
            mm_halow_sched_release();
        }
        if (!mm_halow_in_irq()) {
            // Give the host a chance to run pending events while the driver
            // waits.  The hook must not raise/longjmp out of the driver.
            MM_HALOW_EVENT_POLL_HOOK;
        }
    }
}

// Counted only outside task context: a task parked on a semaphore is the normal
// steady state and would leave this permanently set.
static uint16_t mm_halow_wait_depth;

bool mm_halow_sched_in_wait(void) {
    return mm_halow_wait_depth > 0;
}

bool mm_halow_sched_teardown;

bool mm_halow_sched_wait(mm_halow_cond_fn_t cond, void *arg, uint32_t timeout_ms) {
    // MMOSAL_WAIT_FOREVER is a promise: morselib's SDIO lock path asserts if it
    // returns false, so in normal operation it is honoured and a genuine wedge
    // is the watchdog's problem.  During teardown the promise is capped -- deinit
    // on a dead bus has to complete.
    bool forever = timeout_ms == UINT32_MAX;
    if (timeout_ms > MM_HALOW_SCHED_WAIT_MAX_MS && (!forever || mm_halow_sched_teardown)) {
        timeout_ms = MM_HALOW_SCHED_WAIT_MAX_MS;
    }
    uint32_t start = mm_halow_ticks_ms();
    bool satisfied;
    bool counted = mm_halow_sched_task_current() == NULL;
    if (counted) {
        mm_halow_wait_depth++;
    }
    for (;;) {
        if (cond(arg)) {
            satisfied = true;
            break;
        }
        if (timeout_ms == 0) {
            satisfied = false;
            break;
        }
        if (forever) {
            if (!mm_halow_sched_teardown) {
                mm_halow_sched_yield();
                continue;
            }
            // Teardown began mid-wait: one bounded grace period from here.
            timeout_ms = MM_HALOW_SCHED_WAIT_MAX_MS;
            start = mm_halow_ticks_ms();
            forever = false;
        }
        if ((uint32_t)(mm_halow_ticks_ms() - start) >= timeout_ms) {
            // Re-check once more, in case the condition was satisfied by an
            // interrupt while the deadline was being evaluated.
            satisfied = cond(arg);
            break;
        }
        mm_halow_sched_yield();
    }
    if (counted) {
        mm_halow_wait_depth--;
    }
    return satisfied;
}

void mm_halow_sched_reap(void) {
    mm_halow_task_t **prev = &mm_halow_task_list;
    for (mm_halow_task_t *task = mm_halow_task_list; task != NULL;) {
        mm_halow_task_t *next = task->next;
        if (task->state == MM_HALOW_TASK_DEAD) {
            *prev = next;
            mm_halow_osal_free(task->stack);
            task->stack = NULL;
            task->next = NULL;
        } else {
            prev = &task->next;
        }
        task = next;
    }
}

void mm_halow_sched_run(void) {
    if (mm_halow_sched_running || mm_halow_task_cur != NULL) {
        // Already inside the scheduler, or called from a task.
        return;
    }
    mm_halow_sched_running = true;
    mm_halow_sched_deadline = mm_halow_ticks_ms() + MM_HALOW_SCHED_BUDGET_MS;

    // Several passes per call rather than one.  Handling a frame takes more than
    // one task hop, and with a single pass each hop waits for the next poll,
    // which puts milliseconds of scheduling latency into every round trip.  A
    // task blocked in mm_halow_sched_wait() stays runnable, so this cannot be a
    // loop-until-idle; the pass count is what keeps it from spinning.
    for (int pass = 0; pass < MM_HALOW_SCHED_PASSES && !mm_halow_pass_expired(); pass++) {
        for (mm_halow_task_t *task = mm_halow_task_list; task != NULL; task = task->next) {
            if (task->state != MM_HALOW_TASK_READY) {
                continue;
            }
            mm_halow_task_cur = task;
            #if MM_HALOW_SCHED_SET_MSPLIM
            uint32_t saved_msplim = mm_halow_get_msplim();
            mm_halow_set_msplim((uint32_t)task->stack);
            #endif
            mm_halow_context_switch(&mm_halow_sched_sp, task->sp);
            #if MM_HALOW_SCHED_SET_MSPLIM
            mm_halow_set_msplim(saved_msplim);
            #endif
            mm_halow_task_cur = NULL;
        }
    }

    mm_halow_sched_reap();
    mm_halow_sched_running = false;
}

void mm_halow_sched_deinit(void) {
    // Only safe to call from outside the scheduler; tasks are abandoned where
    // they stand, which is why morselib must be shut down first.  The stacks are
    // not freed individually: the whole pool goes back in mm_halow_osal_deinit().
    mm_halow_task_list = NULL;
    mm_halow_task_cur = NULL;
    mm_halow_sched_running = false;
    mm_halow_service_owner = MM_HALOW_OWNER_NONE;
    mm_halow_service_depth = 0;
    memset(mm_halow_tasks, 0, sizeof(mm_halow_tasks));
}

#endif // MM_HALOW_ENABLED
