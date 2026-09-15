// QEMU-harness configport: the mm_halow_config.h contract, driven by the test.
#ifndef MM_HALOW_QEMUTEST_CONFIGPORT_H
#define MM_HALOW_QEMUTEST_CONFIGPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MM_HALOW_ENABLED (1)

// Single core, no preemption in the harness: the scheduler's atomic sections
// need no masking here.
#define MM_HALOW_BEGIN_ATOMIC_SECTION() (0)
#define MM_HALOW_END_ATOMIC_SECTION(st) ((void)(st))

// Driven by the harness rather than a timer, so wait/timeout behaviour is
// deterministic instead of depending on how fast QEMU happens to run.
extern volatile uint32_t mm_halow_test_ticks;
static inline uint32_t mm_halow_ticks_ms(void) {
    return mm_halow_test_ticks;
}

// Real IPSR: the harness runs on a genuine Cortex-M55 under QEMU, and the
// scheduler's IRQ-context detection is part of what is under test.
#define IPSR_ISR_Msk (0x1FFUL)
static inline uint32_t __get_IPSR(void) {
    uint32_t result;
    __asm volatile ("mrs %0, ipsr" : "=r" (result));
    return result;
}
static inline bool mm_halow_in_irq(void) {
    return (__get_IPSR() & IPSR_ISR_Msk) != 0;
}

// The scheduler sleeps here between turns.  The harness has no tick source of
// its own, so stand in for it by advancing the clock the test drives, and
// count the sleeps so the yield path is observable.
extern volatile uint32_t mm_halow_test_event_waits;
#define MM_HALOW_EVENT_POLL_HOOK                    \
    do {                                            \
        mm_halow_test_ticks += 2;                   \
        mm_halow_test_event_waits++;                \
    } while (0)

// No pins and no printing in the scheduler test.
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
static inline void mm_halow_test_printf_sink(const char *fmt, ...) {
    (void)fmt;
}
#define MM_HALOW_PRINTF(...) mm_halow_test_printf_sink(__VA_ARGS__)
#define MM_HALOW_VPRINTF(fmt, args) ((void)(fmt), (void)(args), 0)

#endif
