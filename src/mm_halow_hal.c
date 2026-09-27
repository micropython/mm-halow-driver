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
// SDIO over SPI
//
// morselib drives the transceiver through the SDIO HAL API (mmhal_wlan_sdio_*),
// and links a default implementation of it built on the byte-level calls above.
// That default moves every framing byte -- idle bytes, response waits, tokens,
// CRC -- through a bus call of its own, each costing several times the byte it
// carries.  The two data-path commands are implemented here instead, whole
// commands and blocks per transfer.  CMD52 and startup, which are rare, keep
// morselib's default.
//
// On the wire a command is an idle byte, the six-byte command with its CRC7, one
// wait byte and a two-byte R5 response.  A read then returns idle bytes and a
// start token (0xfe) before each block, the block and its CRC16.  A write sends a
// start token (0xfe for a single block, 0xfc before each of several), the block
// and its CRC16, and gets back a data response token and a short busy period; a
// multi-block write ends with a stop token (0xfd).  The transceiver treats the
// bus as one stream and carries on across a CS toggle, so command and data go
// out in a single transfer, and a transfer cut short must be clocked to its end
// before the next begins.

#ifndef MM_HALOW_SDIO_OVER_SPI
#define MM_HALOW_SDIO_OVER_SPI (1)
#endif

#if MM_HALOW_SDIO_OVER_SPI

#define MM_HALOW_SDIO_TOKEN_SINGLE      (0xfe)
#define MM_HALOW_SDIO_TOKEN_MULTI       (0xfc)
#define MM_HALOW_SDIO_TOKEN_STOP        (0xfd)

// Idle byte, command, wait byte and R5 response.
#define MM_HALOW_SDIO_CMD_LEN           (10)
#define MM_HALOW_SDIO_BLOCK_MAX         (512)

// Bytes to wait for a late response, data token or end of busy.
#define MM_HALOW_SDIO_RESPONSE_WAIT_MAX (8)
#define MM_HALOW_SDIO_TOKEN_WAIT_MAX    (4096)
#define MM_HALOW_SDIO_BUSY_WAIT_MAX     (200000)

// A read is clocked in chunks of up to this, enough for a full-size frame and
// the gaps between its blocks in one go.
#define MM_HALOW_SDIO_STREAM_MAX        (2112)

// Transfers at least this long run in the background where the port supports it.
#define MM_HALOW_SDIO_ASYNC_MIN         (256)
#define MM_HALOW_SDIO_ASYNC_TIMEOUT_MS  (100)

// Consecutive failed commands before the transport is declared dead.  After a
// corrupted transfer the transceiver's SPI interface can lose its framing for
// good -- a slipped bit, or no response at all -- and morselib then retries
// forever, thousands of times a second, starving the application.
#define MM_HALOW_SDIO_DEAD_AFTER        (32)

static const uint16_t mm_halow_sdio_crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
};

// Command, idle bytes, a block, its CRC and the response tail.
static uint8_t mm_halow_sdio_tx[MM_HALOW_SDIO_BLOCK_MAX + 32] __attribute__((aligned(32)));
// Reads clock out ones and collect the stream here.
static uint8_t mm_halow_sdio_ones[MM_HALOW_SDIO_STREAM_MAX] __attribute__((aligned(32)));
static uint8_t mm_halow_sdio_rx[MM_HALOW_SDIO_STREAM_MAX] __attribute__((aligned(32)));

static uint32_t mm_halow_sdio_fail_run;
static volatile bool mm_halow_sdio_async_done;
bool mm_halow_transport_dead;

static uint16_t mm_halow_sdio_crc16(const uint8_t *p, size_t n, uint16_t crc) {
    while (n--) {
        crc = (crc << 8) ^ mm_halow_sdio_crc16_table[((crc >> 8) ^ *p++) & 0xff];
    }
    return crc;
}

static uint8_t mm_halow_sdio_crc7(const uint8_t *p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        for (int j = 7; j >= 0; j--) {
            uint8_t bit = ((p[i] >> j) & 1) ^ ((crc >> 6) & 1);
            crc = (crc << 1) & 0x7f;
            if (bit) {
                crc ^= 0x09;
            }
        }
    }
    return crc;
}

// Ports that can run an SPI transfer in the background override these; the
// defaults make every transfer a blocking one.
MM_HALOW_WEAK bool mm_halow_port_spi_transfer_start(size_t len, const uint8_t *src, uint8_t *dest) {
    (void)len;
    (void)src;
    (void)dest;
    return false;
}

MM_HALOW_WEAK void mm_halow_port_spi_transfer_finish(bool completed) {
    (void)completed;
}

void mm_halow_hal_spi_async_done(void) {
    mm_halow_sdio_async_done = true;
    mm_halow_schedule_poll();
}

static bool mm_halow_sdio_async_cond(void *arg) {
    (void)arg;
    return mm_halow_sdio_async_done;
}

// A long transfer from a driver task runs in the background: the task sleeps
// until the port reports completion, as with a real SDIO host, and the
// application has the CPU while the bus is busy.  morselib holds its bus lock
// across the whole command, so nothing else reaches the bus meanwhile.
static void mm_halow_sdio_xfer(size_t len, const uint8_t *tx, uint8_t *rx) {
    if (len >= MM_HALOW_SDIO_ASYNC_MIN && mm_halow_sched_task_current() != NULL) {
        mm_halow_sdio_async_done = false;
        if (mm_halow_port_spi_transfer_start(len, tx, rx)) {
            bool done = mm_halow_sched_wait(mm_halow_sdio_async_cond, NULL, MM_HALOW_SDIO_ASYNC_TIMEOUT_MS);
            mm_halow_port_spi_transfer_finish(done);
            return;
        }
    }
    mm_halow_spi_transfer(len, tx, rx);
}

static uint8_t mm_halow_sdio_byte(uint8_t out) {
    uint8_t in = 0xff;
    mm_halow_spi_transfer(1, &out, &in);
    return in;
}

// Clock out idle bytes, discarding what comes back.
static void mm_halow_sdio_drain(size_t n) {
    while (n) {
        size_t chunk = MIN(n, (size_t)MM_HALOW_SDIO_STREAM_MAX);
        mm_halow_sdio_xfer(chunk, mm_halow_sdio_ones, mm_halow_sdio_rx);
        n -= chunk;
    }
}

static void mm_halow_sdio_cmd_frame(uint8_t *t, uint32_t arg) {
    t[0] = 0xff;
    t[1] = 0x40 | 53;
    t[2] = arg >> 24;
    t[3] = arg >> 16;
    t[4] = arg >> 8;
    t[5] = arg;
    t[6] = (mm_halow_sdio_crc7(&t[1], 5) << 1) | 1;
    t[7] = 0xff;
    t[8] = 0xff;
    t[9] = 0xff;
}

// Check the R5 response, which follows the command after at least one wait
// byte.  On success *next is the stream offset just past it.
static int mm_halow_sdio_r5(const uint8_t *rx, size_t len, size_t *next) {
    size_t k = 7;
    while (k + 1 < len && rx[k] == 0xff) {
        k++;
    }
    uint8_t status = rx[k];
    if (status == 0xff) {
        return MMHAL_SDIO_CMD_TIMEOUT;
    }
    if (status & 0x08) {
        return MMHAL_SDIO_CMD_CRC_ERROR;
    }
    // A valid response starts with a zero bit; the rest are error flags, bar the
    // idle bit.
    if (status & 0xd4) {
        return MMHAL_SDIO_OTHER_ERROR;
    }
    *next = k + 2;
    return 0;
}

static bool mm_halow_sdio_never(void *arg) {
    (void)arg;
    return false;
}

// Once the transport is dead, fail without touching the bus, and slowly, so
// that morselib's retries leave the application running.
static int mm_halow_sdio_dead(void) {
    if (mm_halow_sched_task_current() != NULL) {
        mm_halow_sched_wait(mm_halow_sdio_never, NULL, 10);
    }
    return MMHAL_SDIO_HW_ERROR;
}

static int mm_halow_sdio_result(int ret) {
    if (ret == 0) {
        mm_halow_sdio_fail_run = 0;
    } else if (mm_halow_state.booted && !mm_halow_sched_teardown) {
        // Only a transceiver that is up is judged: while it boots, and while it
        // is being shut down, failed commands are expected.
        if (++mm_halow_sdio_fail_run >= MM_HALOW_SDIO_DEAD_AFTER && !mm_halow_transport_dead) {
            mm_halow_transport_dead = true;
            mm_halow_hal_transport_failed();
        }
    }
    return ret;
}

int mmhal_wlan_sdio_cmd53_read(const struct mmhal_wlan_sdio_cmd53_read_args *args) {
    if (mm_halow_transport_dead) {
        return mm_halow_sdio_dead();
    }
    uint32_t bsize = args->block_size ? args->block_size : args->transfer_length;
    uint32_t blocks = args->block_size ? args->transfer_length : 1;
    if (bsize == 0 || bsize > MM_HALOW_SDIO_BLOCK_MAX || blocks == 0) {
        return MMHAL_SDIO_INVALID_ARGUMENT;
    }

    // Clock the command and as much of the reply as is expected in one
    // transfer, then parse the framing out of the buffer.  Reading past the last
    // block is harmless: the transceiver just idles high.
    size_t want = MIN(MM_HALOW_SDIO_CMD_LEN + blocks * (bsize + 6) + 3, (size_t)MM_HALOW_SDIO_STREAM_MAX);
    mm_halow_sdio_cmd_frame(mm_halow_sdio_ones, args->sdio_arg);
    mmhal_wlan_spi_cs_assert();
    mm_halow_sdio_xfer(want, mm_halow_sdio_ones, mm_halow_sdio_rx);
    memset(mm_halow_sdio_ones, 0xff, MM_HALOW_SDIO_CMD_LEN);
    size_t pos = 0;
    size_t avail = want;
    int ret = mm_halow_sdio_r5(mm_halow_sdio_rx, avail, &pos);
    if (ret) {
        // The response may have been corrupted on a command that was in fact
        // accepted: clock out the data that would follow.
        mm_halow_sdio_drain(blocks * (bsize + 8) + 16);
    }

    uint8_t *dest = args->data;
    enum { SDIO_RD_TOKEN, SDIO_RD_DATA, SDIO_RD_CRC } state = SDIO_RD_TOKEN;
    uint32_t need = 0;
    uint32_t skipped = 0;
    uint16_t crc = 0;
    uint16_t got = 0;
    int crc_bytes = 0;
    bool crc_failed = false;
    while (ret == 0 && blocks) {
        if (pos == avail) {
            want = (state == SDIO_RD_DATA ? need + 2 : bsize + 3) + (blocks - 1) * (bsize + 6) + 4;
            want = MIN(want, (size_t)MM_HALOW_SDIO_STREAM_MAX);
            mm_halow_sdio_xfer(want, mm_halow_sdio_ones, mm_halow_sdio_rx);
            avail = want;
            pos = 0;
        }
        if (state == SDIO_RD_TOKEN) {
            uint8_t b = mm_halow_sdio_rx[pos++];
            if (b == MM_HALOW_SDIO_TOKEN_SINGLE) {
                state = SDIO_RD_DATA;
                need = bsize;
                crc = 0;
            } else if (b != 0xff) {
                // An error token, or a start token corrupted on the wire, with
                // the block perhaps still coming: clock the transfer out.
                ret = MMHAL_SDIO_OTHER_ERROR;
                mm_halow_sdio_drain(blocks * (bsize + 8) + 16);
            } else if (++skipped > MM_HALOW_SDIO_TOKEN_WAIT_MAX) {
                ret = MMHAL_SDIO_DATA_TIMEOUT;
            }
        } else if (state == SDIO_RD_DATA) {
            size_t n = MIN(need, avail - pos);
            memcpy(dest, &mm_halow_sdio_rx[pos], n);
            crc = mm_halow_sdio_crc16(dest, n, crc);
            dest += n;
            pos += n;
            need -= n;
            if (need == 0) {
                state = SDIO_RD_CRC;
                crc_bytes = 0;
                got = 0;
            }
        } else {
            got = (got << 8) | mm_halow_sdio_rx[pos++];
            if (++crc_bytes == 2) {
                // Carry on through the remaining blocks after a mismatch: the
                // transceiver is still sending them.
                crc_failed |= got != crc;
                blocks--;
                state = SDIO_RD_TOKEN;
                skipped = 0;
            }
        }
    }
    if (ret == 0 && crc_failed) {
        ret = MMHAL_SDIO_DATA_CRC_ERROR;
    }
    mmhal_wlan_spi_cs_deassert();
    return mm_halow_sdio_result(ret);
}

// End a multi-block write and wait out the busy period that follows.
static void mm_halow_sdio_stop(void) {
    uint8_t out[2] = { MM_HALOW_SDIO_TOKEN_STOP, 0xff };
    uint8_t in[2];
    mm_halow_spi_transfer(2, out, in);
    for (uint32_t w = 0; in[1] != 0xff && w < MM_HALOW_SDIO_BUSY_WAIT_MAX; w++) {
        in[1] = mm_halow_sdio_byte(0xff);
    }
}

int mmhal_wlan_sdio_cmd53_write(const struct mmhal_wlan_sdio_cmd53_write_args *args) {
    if (mm_halow_transport_dead) {
        return mm_halow_sdio_dead();
    }
    bool block_mode = args->block_size != 0;
    uint32_t bsize = block_mode ? args->block_size : args->transfer_length;
    uint32_t blocks = block_mode ? args->transfer_length : 1;
    if (bsize == 0 || bsize > MM_HALOW_SDIO_BLOCK_MAX || blocks == 0) {
        return MMHAL_SDIO_INVALID_ARGUMENT;
    }

    const uint8_t *src = args->data;
    int ret = 0;
    mmhal_wlan_spi_cs_assert();
    for (uint32_t i = 0; i < blocks; i++) {
        bool last = i == blocks - 1;
        uint8_t *t = mm_halow_sdio_tx;
        size_t n = 0;
        if (i == 0) {
            // The command, then two idle bytes before the first data token.
            mm_halow_sdio_cmd_frame(t, args->sdio_arg);
            n = MM_HALOW_SDIO_CMD_LEN;
            t[n++] = 0xff;
            t[n++] = 0xff;
        }
        t[n++] = block_mode ? MM_HALOW_SDIO_TOKEN_MULTI : MM_HALOW_SDIO_TOKEN_SINGLE;
        memcpy(&t[n], src, bsize);
        uint16_t crc = mm_halow_sdio_crc16(src, bsize, 0);
        n += bsize;
        src += bsize;
        t[n++] = crc >> 8;
        t[n++] = crc;
        size_t resp_at = n;
        // The data response token follows within a byte, then a short busy
        // tail.  Clock enough to see both, except before a stop token, which
        // goes out straight after the response.
        size_t tail = (block_mode && last) ? 2 : 4;
        memset(&t[n], 0xff, tail);
        n += tail;
        mm_halow_sdio_xfer(n, t, mm_halow_sdio_rx);

        if (i == 0) {
            size_t next;
            ret = mm_halow_sdio_r5(mm_halow_sdio_rx, MM_HALOW_SDIO_CMD_LEN, &next);
            if (ret) {
                // Should the command have been accepted after all, the
                // transceiver is now part way through a transfer.
                if (block_mode) {
                    mm_halow_sdio_stop();
                }
                break;
            }
        }

        size_t k = resp_at;
        while (k < n && mm_halow_sdio_rx[k] == 0xff) {
            k++;
        }
        uint8_t token = 0xff;
        uint8_t line = 0x00;
        if (k < n) {
            token = mm_halow_sdio_rx[k];
            if (k + 1 < n) {
                line = mm_halow_sdio_rx[n - 1];
            }
        } else {
            for (int w = 0; w < MM_HALOW_SDIO_RESPONSE_WAIT_MAX && token == 0xff; w++) {
                token = mm_halow_sdio_byte(0xff);
            }
        }
        if ((token & 0x1f) != 0x05) {
            ret = (token & 0x1f) == 0x0b ? MMHAL_SDIO_DATA_CRC_ERROR :
                token == 0xff ? MMHAL_SDIO_DATA_TIMEOUT : MMHAL_SDIO_OTHER_ERROR;
            // A rejected block still leaves a multi-block transfer to end.
            if (block_mode) {
                mm_halow_sdio_stop();
            }
            break;
        }
        if (block_mode && last) {
            mm_halow_sdio_stop();
        } else {
            // The transceiver holds the line low while it is busy.
            for (uint32_t w = 0; line != 0xff; w++) {
                if (w >= MM_HALOW_SDIO_BUSY_WAIT_MAX) {
                    ret = MMHAL_SDIO_DATA_TIMEOUT;
                    break;
                }
                line = mm_halow_sdio_byte(0xff);
            }
            if (ret) {
                break;
            }
        }
    }
    mmhal_wlan_spi_cs_deassert();
    return mm_halow_sdio_result(ret);
}

#endif // MM_HALOW_SDIO_OVER_SPI

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
    #if MM_HALOW_SDIO_OVER_SPI
    memset(mm_halow_sdio_ones, 0xff, sizeof(mm_halow_sdio_ones));
    mm_halow_sdio_fail_run = 0;
    mm_halow_transport_dead = false;
    #endif

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
