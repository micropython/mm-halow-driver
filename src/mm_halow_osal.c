/*
 * This file is part of mm-halow-driver.
 *
 * Copyright (c) 2026 OpenMV LLC.
 *
 * SPDX-License-Identifier: MIT
 *
 * MMOSAL implementation.
 *
 * This maps the RTOS abstraction layer that morselib is written against onto
 * the cooperative scheduler in mm_halow_sched.c.  Allocation is served from a
 * dedicated static heap rather than the host's general heap, because morselib
 * allocates from interrupt context where a garbage collector must not run.
 */
#include <stdarg.h>

#include "mm_halow_config.h"

#if MM_HALOW_ENABLED

#include <string.h>

#include "mmosal.h"
#include "mm_halow.h"
#include "mm_halow_sched.h"
#include "mm_halow_osal.h"

// Bytes of heap reserved for morselib.  This covers packet memory as well, so
// it is sized to match the 95 KB heap the Morse reference ports use.
#ifndef MM_HALOW_HEAP_SIZE
#define MM_HALOW_HEAP_SIZE  (96 * 1024)
#endif

/*******************************************************************************/
// Heap

// A first-fit allocator over a static pool.  Blocks are kept in address order in
// a single list so that adjacent free blocks can be coalesced on free.
typedef struct _mm_halow_block_t {
    struct _mm_halow_block_t *next;    // next block, in address order
    size_t size;                    // usable bytes in this block
    bool used;                      // true if handed out to a caller
} mm_halow_block_t;

#define MM_HALOW_BLOCK_ALIGN   (8)
#define MM_HALOW_BLOCK_ROUND(n) (((n) + (MM_HALOW_BLOCK_ALIGN - 1)) & ~(size_t)(MM_HALOW_BLOCK_ALIGN - 1))
#define MM_HALOW_BLOCK_HDR     (MM_HALOW_BLOCK_ROUND(sizeof(mm_halow_block_t)))
#define MM_HALOW_BLOCK_DATA(b) ((void *)((uint8_t *)(b) + MM_HALOW_BLOCK_HDR))
#define MM_HALOW_DATA_BLOCK(p) ((mm_halow_block_t *)((uint8_t *)(p) - MM_HALOW_BLOCK_HDR))

// The pool itself is a single block taken from the host heap when the
// driver is brought up, and held by a root pointer so the GC keeps it alive.
// Sub-allocation out of it is done here rather than by the GC, because morselib
// allocates from PendSV context where the GC must not run.
static mm_halow_block_t *mm_halow_heap_head;

// The backing allocation for the pool, obtained from the host via
// mm_halow_port_heap_alloc() (which is responsible for keeping it alive,
// e.g. registering it as a GC root when the host heap is garbage collected).
static uint8_t *mm_halow_heap_mem;

bool mm_halow_osal_init(void) {
    if (mm_halow_heap_mem != NULL) {
        return true;
    }
    uint8_t *heap = mm_halow_port_heap_alloc(MM_HALOW_HEAP_SIZE);
    if (heap == NULL) {
        return false;
    }
    mm_halow_heap_mem = heap;

    mm_halow_heap_head = (mm_halow_block_t *)heap;
    mm_halow_heap_head->next = NULL;
    mm_halow_heap_head->size = MM_HALOW_HEAP_SIZE - MM_HALOW_BLOCK_HDR;
    mm_halow_heap_head->used = false;
    return true;
}

void *mm_halow_osal_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    // Also stops MM_HALOW_BLOCK_ROUND wrapping to zero for sizes near SIZE_MAX,
    // which would otherwise pass every first-fit test and return a pointer into
    // the block list itself.  Buffer sizes can be derived from received frames.
    if (size > MM_HALOW_HEAP_SIZE) {
        return NULL;
    }
    size = MM_HALOW_BLOCK_ROUND(size);

    if (mm_halow_heap_head == NULL) {
        return NULL;
    }

    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    void *ptr = NULL;
    for (mm_halow_block_t *b = mm_halow_heap_head; b != NULL; b = b->next) {
        if (b->used || b->size < size) {
            continue;
        }
        // Split the block if the remainder can hold a header and something useful.
        if (b->size >= size + MM_HALOW_BLOCK_HDR + MM_HALOW_BLOCK_ALIGN) {
            mm_halow_block_t *split = (mm_halow_block_t *)((uint8_t *)b + MM_HALOW_BLOCK_HDR + size);
            split->next = b->next;
            split->size = b->size - size - MM_HALOW_BLOCK_HDR;
            split->used = false;
            b->next = split;
            b->size = size;
        }
        b->used = true;
        ptr = MM_HALOW_BLOCK_DATA(b);
        break;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return ptr;
}

void mm_halow_osal_free(void *ptr) {
    if (ptr == NULL || mm_halow_heap_head == NULL) {
        return;
    }
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    MM_HALOW_DATA_BLOCK(ptr)->used = false;
    // Coalesce the whole list; it is short and this keeps free O(n) without a
    // back pointer per block.
    for (mm_halow_block_t *b = mm_halow_heap_head; b != NULL && b->next != NULL;) {
        if (!b->used && !b->next->used) {
            b->size += MM_HALOW_BLOCK_HDR + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
}

// Named up here because teardown has to drop the list before the pool the nodes
// live in goes back to the GC heap; the timers themselves are further down.
static struct mmosal_timer *mm_halow_timer_list;

void mm_halow_osal_deinit(void) {
    // Nodes are allocated from the pool that is about to go back to the GC
    // heap.  morselib normally deletes its timers first, but a fatal error or a
    // wedged shutdown does not, and the list outlives mm_halow_init() otherwise.
    mm_halow_timer_list = NULL;
    mm_halow_heap_head = NULL;
    if (mm_halow_heap_mem != NULL) {
        mm_halow_port_heap_free(mm_halow_heap_mem);
        mm_halow_heap_mem = NULL;
    }
}

void *mmosal_malloc_(size_t size) {
    return mm_halow_osal_malloc(size);
}

void *mmosal_calloc_(size_t nitems, size_t size) {
    // nitems * size wraps for large inputs, which would allocate a small block
    // while the caller believes it owns the full product; its first write past
    // the block corrupts the heap. Fail the allocation instead.
    if (nitems != 0 && size > SIZE_MAX / nitems) {
        return NULL;
    }
    size_t total = nitems * size;
    void *ptr = mm_halow_osal_malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *mmosal_realloc_(void *ptr, size_t size) {
    if (ptr == NULL) {
        return mm_halow_osal_malloc(size);
    }
    if (size == 0) {
        mm_halow_osal_free(ptr);
        return NULL;
    }
    size_t old_size = MM_HALOW_DATA_BLOCK(ptr)->size;
    if (old_size >= size) {
        return ptr;
    }
    void *new_ptr = mm_halow_osal_malloc(size);
    if (new_ptr != NULL) {
        memcpy(new_ptr, ptr, old_size);
        mm_halow_osal_free(ptr);
    }
    return new_ptr;
}

void mmosal_free(void *ptr) {
    mm_halow_osal_free(ptr);
}

void *mmosal_malloc_dbg(size_t size, const char *name, unsigned line_number) {
    (void)name;
    (void)line_number;
    return mmosal_malloc_(size);
}

void *mmosal_calloc_dbg(size_t nitems, size_t size, const char *name, unsigned line_number) {
    (void)name;
    (void)line_number;
    return mmosal_calloc_(nitems, size);
}

void *mmosal_realloc_dbg(void *ptr, size_t size, const char *name, unsigned line_number) {
    (void)name;
    (void)line_number;
    return mmosal_realloc_(ptr, size);
}

/*******************************************************************************/
// Tasks

static unsigned mm_halow_critical_nesting;
static uintptr_t mm_halow_critical_state;

struct mmosal_task *mmosal_task_create(mmosal_task_fn_t task_fn, void *argument,
    enum mmosal_task_priority priority, unsigned stack_size_u32, const char *name) {
    // Priorities are ignored: tasks run to their next blocking point in creation
    // order, so there is nothing to prioritise between.
    (void)priority;
    return (struct mmosal_task *)mm_halow_sched_task_create(task_fn, argument, stack_size_u32, name);
}

void mmosal_task_delete(struct mmosal_task *task) {
    mm_halow_sched_task_delete((mm_halow_task_t *)task);
}

struct mmosal_task *mmosal_task_get_active(void) {
    return (struct mmosal_task *)mm_halow_sched_task_current();
}

void mmosal_task_yield(void) {
    mm_halow_sched_yield();
}

static bool mm_halow_deadline_passed(void *arg) {
    return (int32_t)(mm_halow_ticks_ms() - *(uint32_t *)arg) >= 0;
}

void mmosal_task_sleep(uint32_t duration_ms) {
    if (duration_ms == 0) {
        // A zero delay is a yield on an RTOS, not a no-op.
        mm_halow_sched_yield();
        return;
    }
    uint32_t deadline = mm_halow_ticks_ms() + duration_ms;
    mm_halow_sched_wait(mm_halow_deadline_passed, &deadline, duration_ms);
}

// morselib only ever holds a critical section across straight-line work (list
// and counter updates), never across a blocking call. That matters here: the
// nesting count is global rather than per-task, so a task that blocked while
// holding one would leave interrupts disabled for whatever ran next.
void mmosal_task_enter_critical(void) {
    uintptr_t state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    if (mm_halow_critical_nesting++ == 0) {
        mm_halow_critical_state = state;
    }
}

// Whether a critical section is open.  The bus path checks this before giving
// up a turn: yielding here would park the section on the suspended task's stack
// with interrupts still disabled, and the count is global, not per task.
bool mm_halow_osal_in_critical(void) {
    return mm_halow_critical_nesting > 0;
}

void mmosal_task_exit_critical(void) {
    if (mm_halow_critical_nesting > 0 && --mm_halow_critical_nesting == 0) {
        MM_HALOW_END_ATOMIC_SECTION(mm_halow_critical_state);
    }
}

void mmosal_disable_interrupts(void) {
    mmosal_task_enter_critical();
}

void mmosal_enable_interrupts(void) {
    mmosal_task_exit_critical();
}

const char *mmosal_task_name(void) {
    mm_halow_task_t *task = mm_halow_sched_task_current();
    return task != NULL ? task->name : "main";
}

static bool mm_halow_task_is_dead(void *arg) {
    return ((mm_halow_task_t *)arg)->state == MM_HALOW_TASK_DEAD;
}

void mmosal_task_join(struct mmosal_task *task) {
    mm_halow_sched_wait(mm_halow_task_is_dead, task, UINT32_MAX);
}

static bool mm_halow_task_notified(void *arg) {
    mm_halow_task_t *task = arg;
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool notified = task->notify != 0;
    if (notified) {
        task->notify--;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return notified;
}

bool mmosal_task_wait_for_notification(uint32_t timeout_ms) {
    mm_halow_task_t *task = mm_halow_sched_task_current();
    if (task == NULL) {
        // Only tasks can wait for notifications.
        return false;
    }
    return mm_halow_sched_wait(mm_halow_task_notified, task, timeout_ms);
}

void mmosal_task_notify(struct mmosal_task *task) {
    if (task == NULL) {
        return;
    }
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    ((mm_halow_task_t *)task)->notify++;
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
}

void mmosal_task_notify_from_isr(struct mmosal_task *task) {
    mmosal_task_notify(task);
}

/*******************************************************************************/
// Mutexes

struct mmosal_mutex {
    volatile mm_halow_task_t *owner;
    volatile uint32_t taken_ms;
    volatile bool locked;
};

struct mmosal_mutex *mmosal_mutex_create(const char *name) {
    (void)name;
    struct mmosal_mutex *mutex = mm_halow_osal_malloc(sizeof(*mutex));
    if (mutex != NULL) {
        mutex->owner = NULL;
        mutex->locked = false;
    }
    return mutex;
}

void mmosal_mutex_delete(struct mmosal_mutex *mutex) {
    mm_halow_osal_free(mutex);
}

static bool mm_halow_mutex_acquired(void *arg) {
    struct mmosal_mutex *mutex = arg;
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool acquired = !mutex->locked;
    if (acquired) {
        mutex->locked = true;
        mutex->owner = mm_halow_sched_task_current();
        mutex->taken_ms = mm_halow_ticks_ms();
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return acquired;
}

bool mmosal_mutex_get(struct mmosal_mutex *mutex, uint32_t timeout_ms) {
    if (mutex == NULL) {
        return false;
    }
    if (timeout_ms == UINT32_MAX) {
        // A held mutex is normally released in well under a second; nine is a
        // pathology worth naming before settling in to wait it out.
        if (mm_halow_sched_wait(mm_halow_mutex_acquired, mutex, 9000)) {
            return true;
        }
        const mm_halow_task_t *owner = (const mm_halow_task_t *)mutex->owner;
        MM_HALOW_PRINTF("halow: mutex %p slow (owner=%s state=%u held=%ums)\n",
            mutex, owner != NULL ? owner->name : "thread",
            owner != NULL ? (unsigned)owner->state : 0u,
            (unsigned)(mm_halow_ticks_ms() - mutex->taken_ms));
    }
    return mm_halow_sched_wait(mm_halow_mutex_acquired, mutex, timeout_ms);
}

bool mmosal_mutex_release(struct mmosal_mutex *mutex) {
    if (mutex == NULL) {
        return false;
    }
    mutex->owner = NULL;
    mutex->locked = false;
    return true;
}

bool mmosal_mutex_is_held_by_active_task(struct mmosal_mutex *mutex) {
    return mutex != NULL && mutex->locked && mutex->owner == mm_halow_sched_task_current();
}

/*******************************************************************************/
// Semaphores

struct mmosal_sem {
    volatile unsigned count;
    unsigned max_count;
};

struct mmosal_sem *mmosal_sem_create(unsigned max_count, unsigned initial_count, const char *name) {
    (void)name;
    struct mmosal_sem *sem = mm_halow_osal_malloc(sizeof(*sem));
    if (sem != NULL) {
        sem->count = initial_count;
        sem->max_count = max_count;
    }
    return sem;
}

void mmosal_sem_delete(struct mmosal_sem *sem) {
    mm_halow_osal_free(sem);
}

bool mmosal_sem_give(struct mmosal_sem *sem) {
    if (sem == NULL) {
        return false;
    }
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool given = sem->count < sem->max_count;
    if (given) {
        sem->count++;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return given;
}

bool mmosal_sem_give_from_isr(struct mmosal_sem *sem) {
    return mmosal_sem_give(sem);
}

static bool mm_halow_sem_taken(void *arg) {
    struct mmosal_sem *sem = arg;
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool taken = sem->count > 0;
    if (taken) {
        sem->count--;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return taken;
}

bool mmosal_sem_wait(struct mmosal_sem *sem, uint32_t timeout_ms) {
    if (sem == NULL) {
        return false;
    }
    return mm_halow_sched_wait(mm_halow_sem_taken, sem, timeout_ms);
}

uint32_t mmosal_sem_get_count(struct mmosal_sem *sem) {
    return sem != NULL ? sem->count : 0;
}

/*******************************************************************************/
// Binary semaphores

struct mmosal_semb {
    volatile bool signalled;
};

struct mmosal_semb *mmosal_semb_create(const char *name) {
    (void)name;
    struct mmosal_semb *semb = mm_halow_osal_malloc(sizeof(*semb));
    if (semb != NULL) {
        semb->signalled = false;
    }
    return semb;
}

void mmosal_semb_delete(struct mmosal_semb *semb) {
    mm_halow_osal_free(semb);
}

bool mmosal_semb_give(struct mmosal_semb *semb) {
    if (semb == NULL) {
        return false;
    }
    semb->signalled = true;
    return true;
}

bool mmosal_semb_give_from_isr(struct mmosal_semb *semb) {
    return mmosal_semb_give(semb);
}

static bool mm_halow_semb_taken(void *arg) {
    struct mmosal_semb *semb = arg;
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool taken = semb->signalled;
    semb->signalled = false;
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return taken;
}

bool mmosal_semb_wait(struct mmosal_semb *semb, uint32_t timeout_ms) {
    if (semb == NULL) {
        return false;
    }
    return mm_halow_sched_wait(mm_halow_semb_taken, semb, timeout_ms);
}

/*******************************************************************************/
// Queues

struct mmosal_queue {
    size_t num_items;
    size_t item_size;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t count;
    uint8_t *items;
};

struct mmosal_queue *mmosal_queue_create(size_t num_items, size_t item_size, const char *name) {
    (void)name;
    if (item_size != 0 && num_items > (SIZE_MAX - sizeof(struct mmosal_queue)) / item_size) {
        return NULL;
    }
    struct mmosal_queue *queue = mm_halow_osal_malloc(sizeof(*queue) + num_items * item_size);
    if (queue != NULL) {
        queue->num_items = num_items;
        queue->item_size = item_size;
        queue->head = 0;
        queue->tail = 0;
        queue->count = 0;
        queue->items = (uint8_t *)(queue + 1);
    }
    return queue;
}

void mmosal_queue_delete(struct mmosal_queue *queue) {
    mm_halow_osal_free(queue);
}

bool mmosal_queue_pop_from_isr(struct mmosal_queue *queue, void *item) {
    if (queue == NULL) {
        return false;
    }
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool popped = queue->count > 0;
    if (popped) {
        memcpy(item, queue->items + queue->head * queue->item_size, queue->item_size);
        queue->head = (queue->head + 1) % queue->num_items;
        queue->count--;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return popped;
}

bool mmosal_queue_push_from_isr(struct mmosal_queue *queue, const void *item) {
    if (queue == NULL) {
        return false;
    }
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    bool pushed = queue->count < queue->num_items;
    if (pushed) {
        memcpy(queue->items + queue->tail * queue->item_size, item, queue->item_size);
        queue->tail = (queue->tail + 1) % queue->num_items;
        queue->count++;
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return pushed;
}

// Bundles a queue with the item being transferred, for the wait callbacks below.
typedef struct _mm_halow_queue_op_t {
    struct mmosal_queue *queue;
    void *item;
} mm_halow_queue_op_t;

static bool mm_halow_queue_popped(void *arg) {
    mm_halow_queue_op_t *op = arg;
    return mmosal_queue_pop_from_isr(op->queue, op->item);
}

static bool mm_halow_queue_pushed(void *arg) {
    mm_halow_queue_op_t *op = arg;
    return mmosal_queue_push_from_isr(op->queue, op->item);
}

bool mmosal_queue_pop(struct mmosal_queue *queue, void *item, uint32_t timeout_ms) {
    mm_halow_queue_op_t op = { queue, item };
    return mm_halow_sched_wait(mm_halow_queue_popped, &op, timeout_ms);
}

bool mmosal_queue_push(struct mmosal_queue *queue, const void *item, uint32_t timeout_ms) {
    mm_halow_queue_op_t op = { queue, (void *)item };
    return mm_halow_sched_wait(mm_halow_queue_pushed, &op, timeout_ms);
}

/*******************************************************************************/
// Time

uint32_t mmosal_get_time_ms(void) {
    return mm_halow_ticks_ms();
}

uint32_t mmosal_get_time_ticks(void) {
    return mm_halow_ticks_ms();
}

uint32_t mmosal_ticks_per_second(void) {
    return 1000;
}

/*******************************************************************************/
// Timers

struct mmosal_timer {
    struct mmosal_timer *next;
    const char *name;
    uint32_t period_ms;
    uint32_t expires_at;
    bool auto_reload;
    volatile bool active;
    void *arg;
    timer_callback_t callback;
};

struct mmosal_timer *mmosal_timer_create(const char *name, uint32_t timer_period_ms,
    bool auto_reload, void *arg, timer_callback_t callback) {
    struct mmosal_timer *timer = mm_halow_osal_malloc(sizeof(*timer));
    if (timer == NULL) {
        return NULL;
    }
    timer->name = name;
    timer->period_ms = timer_period_ms;
    timer->expires_at = 0;
    timer->auto_reload = auto_reload;
    timer->active = false;
    timer->arg = arg;
    timer->callback = callback;

    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    timer->next = mm_halow_timer_list;
    mm_halow_timer_list = timer;
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    return timer;
}

void mmosal_timer_delete(struct mmosal_timer *timer) {
    if (timer == NULL) {
        return;
    }
    uintptr_t atomic_state = MM_HALOW_BEGIN_ATOMIC_SECTION();
    for (struct mmosal_timer **t = &mm_halow_timer_list; *t != NULL; t = &(*t)->next) {
        if (*t == timer) {
            *t = timer->next;
            break;
        }
    }
    MM_HALOW_END_ATOMIC_SECTION(atomic_state);
    mm_halow_osal_free(timer);
}

bool mmosal_timer_start(struct mmosal_timer *timer) {
    if (timer == NULL) {
        return false;
    }
    timer->expires_at = mm_halow_ticks_ms() + timer->period_ms;
    timer->active = true;
    return true;
}

bool mmosal_timer_stop(struct mmosal_timer *timer) {
    if (timer == NULL) {
        return false;
    }
    timer->active = false;
    return true;
}

bool mmosal_timer_change_period(struct mmosal_timer *timer, uint32_t new_period) {
    if (timer == NULL) {
        return false;
    }
    timer->period_ms = new_period;
    if (timer->active) {
        timer->expires_at = mm_halow_ticks_ms() + new_period;
    }
    return true;
}

void *mmosal_timer_get_arg(struct mmosal_timer *timer) {
    return timer != NULL ? timer->arg : NULL;
}

bool mmosal_is_timer_active(struct mmosal_timer *timer) {
    return timer != NULL && timer->active;
}

void mm_halow_osal_timer_poll(void) {
    uint32_t now = mm_halow_ticks_ms();
    struct mmosal_timer *timer = mm_halow_timer_list;
    while (timer != NULL) {
        // Read before the callback: deleting the timer from inside its own
        // callback is allowed, and that frees the node standing here.
        struct mmosal_timer *next = timer->next;
        if (timer->active && (int32_t)(now - timer->expires_at) >= 0) {
            if (timer->auto_reload) {
                timer->expires_at = now + timer->period_ms;
            } else {
                timer->active = false;
            }
            timer->callback(timer);
        }
        timer = next;
    }
}

/*******************************************************************************/
// Failure handling

int mmosal_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = MM_HALOW_VPRINTF(format, args);
    va_end(args);
    return ret;
}

// The console dies with many of the faults this reports, so the record also
// lands in RAM where a debug probe can read it post-mortem.
struct mm_halow_fatal_record mm_halow_fatal_record;

void mmosal_log_failure_info(const struct mmosal_failure_info *info) {
    mm_halow_fatal_record.pc = info->pc;
    mm_halow_fatal_record.lr = info->lr;
    mm_halow_fatal_record.fileid = info->fileid;
    mm_halow_fatal_record.line = info->line;
    mm_halow_fatal_record.ticks_ms = mm_halow_ticks_ms();
    mm_halow_fatal_record.magic = MM_HALOW_FATAL_MAGIC;
    MM_HALOW_PRINTF("halow: failure at pc=0x%08x lr=0x%08x file=%u line=%u\n",
        (unsigned int)info->pc, (unsigned int)info->lr,
        (unsigned int)info->fileid, (unsigned int)info->line);
}

bool mmosal_extract_failure_info(struct mmosal_failure_info *buf, uint32_t *failure_count) {
    (void)buf;
    if (failure_count != NULL) {
        *failure_count = 0;
    }
    return false;
}

void mmosal_impl_assert(void) {
    // Hand off to the port's assert/fatal-error handling; it must not return.
    mm_halow_port_assert_fail();
    for (;;) {
    }
}

int mmosal_main(mmosal_app_init_cb_t app_init_cb) {
    // The host owns main(); morselib is driven from mm_halow_poll().
    (void)app_init_cb;
    return -1;
}

#endif // MM_HALOW_ENABLED
