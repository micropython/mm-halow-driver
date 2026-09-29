/*
 * This file is part of mm-halow-driver.
 *
 * Copyright (c) 2026 OpenMV LLC.
 *
 * SPDX-License-Identifier: MIT
 *
 * Driver-internal API: shared between the driver's own source files, and
 * reachable from its tests, but not part of what ports and bindings use.
 */

#ifndef MM_HALOW_INCLUDED_HALOW_INTERNAL_H
#define MM_HALOW_INCLUDED_HALOW_INTERNAL_H

#include <stdbool.h>

// Ask for a poll that runs in full whatever the scheduler has pending: for a
// queued transmit or a transceiver interrupt.  Wraps mm_halow_schedule_poll().
void mm_halow_request_poll(void);

/*******************************************************************************/
// HAL hooks, implemented in mm_halow_hal.c

// Poll the transceiver's interrupt lines.
void mm_halow_hal_poll_irqs(void);

// True when the transceiver has raised something since the last poll: IRQ
// asserted, or the BUSY line changed.
bool mm_halow_hal_irq_pending(void);

// Re-enable the transceiver's pin interrupt after a poll has drained it.
void mm_halow_hal_irq_rearm(void);

#endif // MM_HALOW_INCLUDED_HALOW_INTERNAL_H
