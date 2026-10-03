// Driver configuration for the QEMU test (see mm_halow_config.h).  The pins,
// clock and poll hook are wired to the test and emulated transceiver in
// test_bringup.c.
#ifndef MM_HALOW_QEMU_DRIVER_CONFIGPORT_H
#define MM_HALOW_QEMU_DRIVER_CONFIGPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MM_HALOW_ENABLED (1)

// Normally supplied by the host.
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define SIZEOF_ETH_HDR (14)

// Single core and nothing preempts the driver, so there is nothing to mask.
#define MM_HALOW_BEGIN_ATOMIC_SECTION() (0)
#define MM_HALOW_END_ATOMIC_SECTION(state) ((void)(state))

// Driven by the test rather than a timer, so waits and timeouts are
// deterministic instead of depending on how fast QEMU runs.
extern volatile uint32_t mm_halow_test_ticks;
static inline uint32_t mm_halow_ticks_ms(void) {
    return mm_halow_test_ticks;
}

// The real IPSR: the core is a genuine Cortex-M55 under QEMU.
static inline bool mm_halow_in_irq(void) {
    uint32_t ipsr;
    __asm volatile ("mrs %0, ipsr" : "=r" (ipsr));
    return (ipsr & 0x1ff) != 0;
}

// Pins.
typedef int mm_halow_pin_t;
#define MM_HALOW_CS (0)
#define MM_HALOW_BUSY (1)
#define MM_HALOW_WAKE (2)
#define MM_HALOW_RESET (3)
#define MM_HALOW_IRQ (4)
bool mm_halow_test_pin_read(mm_halow_pin_t pin);
void mm_halow_test_pin_write(mm_halow_pin_t pin, bool value);
static inline bool mm_halow_hal_pin_read(mm_halow_pin_t pin) {
    return mm_halow_test_pin_read(pin);
}
static inline void mm_halow_hal_pin_write(mm_halow_pin_t pin, bool value) {
    mm_halow_test_pin_write(pin, value);
}
static inline void mm_halow_hal_pin_input(mm_halow_pin_t pin) {
}
static inline void mm_halow_hal_pin_output(mm_halow_pin_t pin) {
}

// Stands in for the host's network poll, which is what services the
// transceiver while the driver waits, and lets time pass.
void mm_halow_test_event_poll(void);
#define MM_HALOW_EVENT_POLL_HOOK mm_halow_test_event_poll()

#endif
