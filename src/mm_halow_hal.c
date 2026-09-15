/*
 * This file is part of mm-halow-driver.
 *
 * Copyright (c) 2026 OpenMV LLC.
 *
 * SPDX-License-Identifier: MIT
 *
 * MMHAL implementation for MicroPython: the SD-over-SPI transport to the Morse
 * Micro transceiver, plus the firmware and board-configuration blobs.
 */
#include "mm_halow_config.h"

#if MM_HALOW_ENABLED

#include <string.h>

#include "mmhal.h"
#include "mmhal_wlan.h"
#include "mmosal.h"
#include "mm_halow.h"
#include "mm_halow_osal.h"
#include "mm_halow_sched.h"

// Bytes clocked out with MOSI held high to stabilise the transceiver's SD-over-SPI
// state machine.  Must be at least 74 bits, see section 6.4.1.1 of "SD Physical
// Layer Simplified Specification Version 9.10".
#define MM_HALOW_TRAINING_BYTES    (16)

// Size of the scratch buffer used to drive MOSI high during read transfers.
// Reads are chunked through it so that the driver never allocates per transfer.
#define MM_HALOW_READ_CHUNK        (128)

static mmhal_irq_handler_t mm_halow_spi_irq_handler;
static mmhal_irq_handler_t mm_halow_busy_irq_handler;
static volatile bool mm_halow_spi_irq_enabled;
static volatile bool mm_halow_busy_irq_enabled;

// Filled with 0xff at init and never written again: SD-over-SPI needs MOSI held
// high while reading, and clocking out of a fixed buffer keeps reads allocation
// free.
static uint8_t mm_halow_spi_ones[MM_HALOW_READ_CHUNK];

// The firmware image and, if the board supplies one, the board configuration
// file.  Both are linked in as binary blobs, see extmod.mk.
extern uint8_t mm_halow_firmware_start;
extern uint8_t mm_halow_firmware_end;
#ifdef MM_HALOW_BCF
extern uint8_t mm_halow_bcf_start;
extern uint8_t mm_halow_bcf_end;
#endif

/*******************************************************************************/
// Bus

static void mm_halow_spi_transfer(size_t len, const uint8_t *src, uint8_t *dest) {
    mm_halow_port_spi_transfer(len, src, dest);
}

void mmhal_wlan_spi_cs_assert(void) {
    // The gap before a transaction starts is the one point in the bus path
    // where nothing is in flight, so it is where a task that has overrun its
    // turn is made to give one up.  A retry loop re-asserts CS every time round,
    // so this is always reached however long morselib intends to keep trying.
    if (mm_halow_sched_over_budget() && !mm_halow_osal_in_critical()) {
        mm_halow_sched_yield();
    }
    mm_halow_hal_pin_write(MM_HALOW_CS, 0);
}

void mmhal_wlan_spi_cs_deassert(void) {
    mm_halow_hal_pin_write(MM_HALOW_CS, 1);
}

uint8_t mmhal_wlan_spi_rw(uint8_t data) {
    uint8_t rx = 0xff;
    mm_halow_spi_transfer(1, &data, &rx);
    return rx;
}

void mmhal_wlan_spi_read_buf(uint8_t *buf, unsigned len) {
    // SD-over-SPI requires MOSI to be held high while reading, so clock out ones
    // from a fixed scratch buffer rather than doing a receive-only transfer.
    while (len > 0) {
        size_t chunk = MIN(len, MM_HALOW_READ_CHUNK);
        mm_halow_spi_transfer(chunk, mm_halow_spi_ones, buf);
        buf += chunk;
        len -= chunk;
    }
}

void mmhal_wlan_spi_write_buf(const uint8_t *buf, unsigned len) {
    mm_halow_spi_transfer(len, buf, NULL);
}

void mmhal_wlan_send_training_seq(void) {
    mmhal_wlan_spi_cs_deassert();
    mm_halow_spi_transfer(MM_HALOW_TRAINING_BYTES, mm_halow_spi_ones, NULL);
}

/*******************************************************************************/
// Control lines

void mmhal_wlan_assert_reset(bool assert_reset) {
    mm_halow_hal_pin_write(MM_HALOW_RESET, assert_reset ? 0 : 1);
}

void mmhal_wlan_hard_reset(void) {
    mmhal_wlan_assert_reset(true);
    mmosal_task_sleep(5);
    mmhal_wlan_assert_reset(false);
    mmosal_task_sleep(20);
}

void mmhal_wlan_wake_assert(void) {
    mm_halow_hal_pin_write(MM_HALOW_WAKE, 1);
}

void mmhal_wlan_wake_deassert(void) {
    mm_halow_hal_pin_write(MM_HALOW_WAKE, 0);
}

bool mmhal_wlan_busy_is_asserted(void) {
    // The transceiver drives BUSY high, but a board may invert it on the way to
    // the MCU, for example to share the line with a wake-up input.
    #if MM_HALOW_BUSY_INVERTED
    return mm_halow_hal_pin_read(MM_HALOW_BUSY) == 0;
    #else
    return mm_halow_hal_pin_read(MM_HALOW_BUSY) != 0;
    #endif
}

void mmhal_wlan_register_busy_irq_handler(mmhal_irq_handler_t handler) {
    mm_halow_busy_irq_handler = handler;
}

void mmhal_wlan_set_busy_irq_enabled(bool enabled) {
    mm_halow_busy_irq_enabled = enabled;
}

bool mmhal_wlan_spi_irq_is_asserted(void) {
    // The transceiver drives the line low while it has data pending.
    return mm_halow_hal_pin_read(MM_HALOW_IRQ) == 0;
}

void mmhal_wlan_clear_spi_irq(void) {
    // The line is level driven by the transceiver, so there is nothing to clear.
}

void mmhal_wlan_register_spi_irq_handler(mmhal_irq_handler_t handler) {
    mm_halow_spi_irq_handler = handler;
}

void mmhal_wlan_set_spi_irq_enabled(bool enabled) {
    mm_halow_spi_irq_enabled = enabled;
    // The line is level, not edge, driven: if it is already asserted when the
    // interrupt is enabled there will be no further edge to trigger on.
    if (enabled && mmhal_wlan_spi_irq_is_asserted() && mm_halow_spi_irq_handler != NULL) {
        mm_halow_spi_irq_handler();
    }
}

#if MM_HALOW_ENABLE_PIN_IRQ
// The transceiver holds IRQ low until it is serviced, so the falling edge is
// the assert.  Waking the driver from the edge rather than waiting for the
// next network poll shaves a poll period off a round trip, which crosses that
// wait twice.  Optional: a port that cannot register an edge interrupt on the
// IRQ pin builds with MM_HALOW_ENABLE_PIN_IRQ disabled and just waits for the next
// poll -- mm_halow_hal_poll_irqs() reads the line by level anyway.
//
// The port's interrupt implementation must call this handler on the falling
// edge of the IRQ line.
void mm_halow_port_irq_handler(void) {
    // Coalesce: one poll drains everything the transceiver has, so further
    // edges until then are pure overhead.  Re-armed by mm_halow_hal_irq_rearm().
    mm_halow_port_irq_enable(false);
    mm_halow_schedule_poll();
}

void mm_halow_hal_irq_rearm(void) {
    mm_halow_port_irq_enable(true);
}
#else
void mm_halow_hal_irq_rearm(void) {
}
#endif

// Called from mm_halow_poll() to pick up transceiver interrupts.  Level-checking
// here rather than relying purely on a pin interrupt keeps the driver correct on
// boards where the IRQ line is not wired to an interrupt-capable pin.
void mm_halow_hal_poll_irqs(void) {
    if (mm_halow_spi_irq_enabled && mm_halow_spi_irq_handler != NULL && mmhal_wlan_spi_irq_is_asserted()) {
        mm_halow_spi_irq_handler();
    }
    if (mm_halow_busy_irq_enabled && mm_halow_busy_irq_handler != NULL && mmhal_wlan_busy_is_asserted()) {
        mm_halow_busy_irq_handler();
    }
}

/*******************************************************************************/
// Init

void mmhal_wlan_init(void) {
    memset(mm_halow_spi_ones, 0xff, sizeof(mm_halow_spi_ones));

    mm_halow_hal_pin_output(MM_HALOW_RESET);
    mm_halow_hal_pin_write(MM_HALOW_RESET, 0);
    mm_halow_hal_pin_output(MM_HALOW_WAKE);
    mm_halow_hal_pin_write(MM_HALOW_WAKE, 0);
    mm_halow_hal_pin_output(MM_HALOW_CS);
    mm_halow_hal_pin_write(MM_HALOW_CS, 1);
    mm_halow_hal_pin_input(MM_HALOW_BUSY);
    mm_halow_hal_pin_input(MM_HALOW_IRQ);
    #if MM_HALOW_ENABLE_PIN_IRQ
    mm_halow_port_irq_config(true);
    #endif

    // Mode 0, MSB first: the transceiver's SD-over-SPI interface samples on the
    // rising edge with the clock idling low.
    mm_halow_port_spi_init();

    // Initialising the SPI peripheral may have reclaimed the CS pin.
    mm_halow_hal_pin_output(MM_HALOW_CS);
    mm_halow_hal_pin_write(MM_HALOW_CS, 1);

    mmhal_wlan_assert_reset(false);
}

void mmhal_wlan_deinit(void) {
    #if MM_HALOW_ENABLE_PIN_IRQ
    mm_halow_port_irq_config(false);
    #endif
    mm_halow_spi_irq_enabled = false;
    mm_halow_busy_irq_enabled = false;
    mm_halow_spi_irq_handler = NULL;
    mm_halow_busy_irq_handler = NULL;

    mmhal_wlan_assert_reset(true);
    mm_halow_hal_pin_write(MM_HALOW_WAKE, 0);
    mm_halow_port_spi_deinit();
}

#if defined(MM_HALOW_EXT_XTAL_INIT) && MM_HALOW_EXT_XTAL_INIT
bool mmhal_wlan_ext_xtal_init_is_required(void) {
    return true;
}
#endif

const struct mmhal_chip *mmhal_get_chip(void) {
    return &MM_HALOW_CHIPSET;
}

/*******************************************************************************/
// Firmware and board configuration blobs

static void mm_halow_read_blob(const uint8_t *start, const uint8_t *end,
    uint32_t offset, uint32_t requested_len, struct mmhal_robuf *robuf) {
    robuf->buf = NULL;
    robuf->len = 0;
    robuf->free_arg = NULL;
    robuf->free_cb = NULL;

    size_t len = end - start;
    if (offset > len) {
        return;
    }
    robuf->buf = (uint8_t *)start + offset;
    robuf->len = MIN(len - offset, requested_len);
}

void mmhal_wlan_read_fw_file(uint32_t offset, uint32_t requested_len, struct mmhal_robuf *robuf) {
    mm_halow_read_blob(&mm_halow_firmware_start, &mm_halow_firmware_end, offset, requested_len, robuf);
}

void mmhal_wlan_read_bcf_file(uint32_t offset, uint32_t requested_len, struct mmhal_robuf *robuf) {
    #ifdef MM_HALOW_BCF
    mm_halow_read_blob(&mm_halow_bcf_start, &mm_halow_bcf_end, offset, requested_len, robuf);
    #else
    // Without a board configuration file the transceiver falls back to the
    // calibration data in its own OTP.
    (void)offset;
    (void)requested_len;
    robuf->buf = NULL;
    robuf->len = 0;
    robuf->free_arg = NULL;
    robuf->free_cb = NULL;
    #endif
}

/*******************************************************************************/
// Miscellaneous

void mmhal_read_mac_addr(uint8_t *mac_addr) {
    // Leave whatever the transceiver reported from its OTP in place; if that is
    // all zeroes, derive a stable locally administered address from the MCU's
    // unique ID so that the same board always joins with the same address.
    for (int i = 0; i < 6; i++) {
        if (mac_addr[i] != 0) {
            return;
        }
    }

    mm_halow_port_get_mac(mac_addr);
}

uint32_t mmhal_random_u32(uint32_t min, uint32_t max) {
    uint32_t value = mm_halow_port_random_u32();
    if (max <= min) {
        return min;
    }
    uint32_t span = max - min;
    if (span == UINT32_MAX) {
        // The whole range: the count of values is 2^32, which does not fit, and
        // computing it wraps to zero.  morselib asks for exactly this when it
        // needs random bytes, and the modulo by zero left every one of them 0.
        return value;
    }
    return min + value % (span + 1);
}

void mmhal_set_deep_sleep_veto(uint8_t veto_id) {
    (void)veto_id;
}

void mmhal_clear_deep_sleep_veto(uint8_t veto_id) {
    (void)veto_id;
}

#endif // MM_HALOW_ENABLED
