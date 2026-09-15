/*
 * This file is part of mm-halow-driver.
 *
 * Copyright (c) 2026 OpenMV LLC.
 *
 * SPDX-License-Identifier: MIT
 *
 * Host integration contract for the mm-halow driver: everything the driver
 * needs from the embedding environment is declared here, and the embedder
 * provides it in mm_halow_configport.h (or the file named by
 * MM_HALOW_CONFIG_FILE).
 */
#ifndef MM_HALOW_INCLUDED_CONFIG_H
#define MM_HALOW_INCLUDED_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Import port-specific configuration file.
#ifdef MM_HALOW_CONFIG_FILE
#include MM_HALOW_CONFIG_FILE
#else
#include <mm_halow_configport.h>
#endif

/*******************************************************************************/
// Driver options.  Defaults here; a port overrides them in its config file.

// Access point mode.  morselib's AP support is an alpha API and
// mmwlan_ap_enable() does not currently succeed on the MM8108, so the mode is
// built out rather than offered and failing.
#ifndef MM_HALOW_ENABLE_AP
#define MM_HALOW_ENABLE_AP (0)
#endif

// Optional falling-edge interrupt on the IRQ line.  The line is read by level
// on every poll regardless, so a port without an edge-interrupt-capable IRQ
// pin just leaves this off and waits for the next poll.
#ifndef MM_HALOW_ENABLE_PIN_IRQ
#define MM_HALOW_ENABLE_PIN_IRQ (0)
#endif

// The morselib chipset descriptor to bind to (mmhal_mm8108, ...).
#ifndef MM_HALOW_CHIPSET
#define MM_HALOW_CHIPSET   mmhal_mm8108
#endif

// SPI clock.  At 50MHz the SD-over-SPI framing corrupts under sustained traffic
// and the transceiver stops answering, so the default is conservative.  Note
// this is a request, not a setting: stm32 and mimxrt round down to the nearest
// rate, but alif truncates clk / speed and DesignWare SSI forces the divider
// even, so an unevenly-dividing request can yield a *faster* bus (40MHz becomes
// 50MHz on a 200MHz AHB) -- check the rate the peripheral actually produced.
#ifndef MM_HALOW_SPI_BAUDRATE
#define MM_HALOW_SPI_BAUDRATE   (25000000)
#endif

// Set by boards that invert the transceiver's BUSY output before it reaches the
// MCU.  RESET_N is active low at the transceiver and is not configurable.
#ifndef MM_HALOW_BUSY_INVERTED
#define MM_HALOW_BUSY_INVERTED (0)
#endif

// IP MTU.  morselib accepts frames up to MMHAL_WLAN_MMPKT_TX_MAX_SIZE, but
// 802.11ah carries ordinary Ethernet traffic so the usual 1500 applies.
#ifndef MM_HALOW_MTU
#define MM_HALOW_MTU    (1500)
#endif

// Address the station comes up on when the build has no DHCP client.  Unused
// otherwise, as the lease supplies all three.
#ifndef MM_HALOW_STA_ADDRESS
#define MM_HALOW_STA_ADDRESS    (0xc0a80102)    // 192.168.1.2
#endif
#ifndef MM_HALOW_STA_NETMASK
#define MM_HALOW_STA_NETMASK    (0xffffff00)    // 255.255.255.0
#endif
#ifndef MM_HALOW_STA_GATEWAY
#define MM_HALOW_STA_GATEWAY    (0xc0a80101)    // 192.168.1.1
#endif

// Address the soft AP hands out, if the board does not override it.
#ifndef MM_HALOW_AP_ADDRESS
#define MM_HALOW_AP_ADDRESS (0xc0a80401)    // 192.168.4.1
#endif
#ifndef MM_HALOW_AP_NETMASK
#define MM_HALOW_AP_NETMASK (0xffffff00)    // 255.255.255.0
#endif

// Networks one scan can report.  A sweep that finds more than this drops the
// rest, so it is the ceiling on what scan() can return.
#ifndef MM_HALOW_SCAN_CACHE_MAX
#define MM_HALOW_SCAN_CACHE_MAX    (32)
#endif

/*******************************************************************************/
// Hooks the port must provide (no usable defaults).

// Mask/restore interrupts around the driver's short critical sections.
// MM_HALOW_BEGIN_ATOMIC_SECTION() returns an opaque state that is passed back
// to MM_HALOW_END_ATOMIC_SECTION().  These stay macros so the port can
// save/restore its interrupt state inline.
#ifndef MM_HALOW_BEGIN_ATOMIC_SECTION
#error "port must define MM_HALOW_BEGIN_ATOMIC_SECTION/MM_HALOW_END_ATOMIC_SECTION"
#endif

// A free-running millisecond tick counter.
uint32_t mm_halow_ticks_ms(void);

// True when executing in interrupt context (on Cortex-M: IPSR != 0).
bool mm_halow_in_irq(void);

// GPIO accessors, applied to the MM_HALOW_CS/RESET/WAKE/BUSY/IRQ pin values the
// port defines.  The port also defines mm_halow_pin_t, its native pin handle
// (the type of those MM_HALOW_* values).
bool mm_halow_hal_pin_read(mm_halow_pin_t pin);
void mm_halow_hal_pin_write(mm_halow_pin_t pin, bool value);
void mm_halow_hal_pin_input(mm_halow_pin_t pin);
void mm_halow_hal_pin_output(mm_halow_pin_t pin);

/*******************************************************************************/
// Hooks with defaults.

// Runs while the driver busy-waits, so the host can service its own pending
// events.  It must not raise/longjmp out of the driver.
#ifndef MM_HALOW_EVENT_POLL_HOOK
#define MM_HALOW_EVENT_POLL_HOOK
#endif

// Diagnostic output.
#ifndef MM_HALOW_PRINTF
#include <stdio.h>
#define MM_HALOW_PRINTF(...) printf(__VA_ARGS__)
#endif
#ifndef MM_HALOW_VPRINTF
#include <stdarg.h>
#include <stdio.h>
#define MM_HALOW_VPRINTF(fmt, args) vprintf(fmt, args)
#endif

#ifndef MM_HALOW_WEAK
#define MM_HALOW_WEAK __attribute__((weak))
#endif

// Error codes returned by the driver API (negated).  Default to the C library
// values; an embedder with its own errno space overrides these.
#ifndef MM_HALOW_EPERM
#include <errno.h>
#define MM_HALOW_EPERM      EPERM
#define MM_HALOW_EIO        EIO
#define MM_HALOW_EINVAL     EINVAL
#define MM_HALOW_EAGAIN     EAGAIN
#define MM_HALOW_ENOMEM     ENOMEM
#define MM_HALOW_ENODEV     ENODEV
#define MM_HALOW_ENOENT     ENOENT
#define MM_HALOW_ENOTCONN   ENOTCONN
#define MM_HALOW_ENXIO      ENXIO
#define MM_HALOW_ETIMEDOUT  ETIMEDOUT
#define MM_HALOW_ERANGE     ERANGE
#define MM_HALOW_EOPNOTSUPP EOPNOTSUPP
#endif

// Hostname reported to the DHCP server (a char pointer or array).
#ifndef MM_HALOW_HOST_NAME
#define MM_HALOW_HOST_NAME "mm-halow"
#endif

/*******************************************************************************/
// Functions the port must implement (see README.md).

// SPI bus: mode 0, MSB first, MM_HALOW_SPI_BAUDRATE.  The chip select is a
// plain GPIO (MM_HALOW_CS) driven by the driver, not by the SPI peripheral.
void mm_halow_port_spi_init(void);
void mm_halow_port_spi_deinit(void);
void mm_halow_port_spi_transfer(size_t len, const uint8_t *src, uint8_t *dest);

// The backing memory for the driver's private heap (MM_HALOW_HEAP_SIZE bytes).
// The port owns keeping the allocation alive (e.g. registering it as a GC root
// on a garbage-collected host).
uint8_t *mm_halow_port_heap_alloc(size_t size);
void mm_halow_port_heap_free(uint8_t *ptr);

// A hardware random 32-bit value.
uint32_t mm_halow_port_random_u32(void);

// Called when the driver hits an unrecoverable internal failure (a morselib
// assertion).  The port routes this to its own assert/fatal-error handling; it
// must not return.
void mm_halow_port_assert_fail(void);

// Fallback station MAC address, used only when the transceiver's OTP holds
// none.  Must be stable across boots.
void mm_halow_port_get_mac(uint8_t mac_addr[6]);

#if MM_HALOW_ENABLE_PIN_IRQ
// Optional falling-edge interrupt on the IRQ line.  The port's ISR must call
// mm_halow_port_irq_handler(); the driver enables/disables delivery with
// mm_halow_port_irq_enable() to coalesce bursts.
void mm_halow_port_irq_config(bool enabled);
void mm_halow_port_irq_enable(bool enabled);
void mm_halow_port_irq_handler(void);
#endif

#endif // MM_HALOW_INCLUDED_CONFIG_H
