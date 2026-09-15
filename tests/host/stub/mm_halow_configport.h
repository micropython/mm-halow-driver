// Host-test configport: the mm_halow_config.h contract implemented with
// instrumented counters, so the REAL driver sources compile natively and the
// tests can assert on the integration behavior (atomic balance, heap failure
// handling) instead of copies that would drift.
#ifndef MM_HALOW_HOSTTEST_CONFIGPORT_H
#define MM_HALOW_HOSTTEST_CONFIGPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MM_HALOW_ENABLED (1)

// The driver takes an atomic section around every heap operation because
// morselib allocates from PendSV. On the host there is no preemption, but the
// test counts nesting to prove the sections are balanced -- an unbalanced
// section on target leaves interrupts disabled forever.
extern int mm_halow_test_atomic_depth;
extern int mm_halow_test_atomic_max;

static inline uintptr_t mm_halow_test_begin_atomic(void) {
    mm_halow_test_atomic_depth++;
    if (mm_halow_test_atomic_depth > mm_halow_test_atomic_max) {
        mm_halow_test_atomic_max = mm_halow_test_atomic_depth;
    }
    return 0;
}

static inline void mm_halow_test_end_atomic(uintptr_t state) {
    (void)state;
    mm_halow_test_atomic_depth--;
}

#define MM_HALOW_BEGIN_ATOMIC_SECTION() mm_halow_test_begin_atomic()
#define MM_HALOW_END_ATOMIC_SECTION(st) mm_halow_test_end_atomic(st)

extern uint32_t mm_halow_test_ticks_ms;
static inline uint32_t mm_halow_ticks_ms(void) {
    return mm_halow_test_ticks_ms;
}

// The port's native pin handle; the allocator tests exercise no pins, so the
// accessors are no-ops.
typedef int mm_halow_pin_t;
static inline bool mm_halow_hal_pin_read(mm_halow_pin_t pin) {
    (void)pin;
    return false;
}
static inline void mm_halow_hal_pin_write(mm_halow_pin_t pin, bool value) {
    (void)pin;
    (void)value;
}
static inline void mm_halow_hal_pin_input(mm_halow_pin_t pin) {
    (void)pin;
}
static inline void mm_halow_hal_pin_output(mm_halow_pin_t pin) {
    (void)pin;
}

// The host harness never runs from interrupt context.
static inline bool mm_halow_in_irq(void) {
    return false;
}

// Keep test output clean: the OSAL log wrapper routes here.  The sink still
// evaluates its arguments so diagnostics-only variables stay referenced.
static inline void mm_halow_test_printf_sink(const char *fmt, ...) {
    (void)fmt;
}
#define MM_HALOW_PRINTF(...) mm_halow_test_printf_sink(__VA_ARGS__)
#define MM_HALOW_VPRINTF(fmt, args) ((void)(fmt), (void)(args), 0)

#endif
