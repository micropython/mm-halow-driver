/*
 * This file is part of mm-halow-driver.
 *
 * Copyright (c) 2026 OpenMV LLC.
 *
 * SPDX-License-Identifier: MIT
 *
 * Compiler and architecture hooks required by morselib.
 */
#ifndef MM_HALOW_INCLUDED_MMPORT_H
#define MM_HALOW_INCLUDED_MMPORT_H

#define MMPORT_BREAKPOINT() __asm("bkpt 0\n\t")
#define MMPORT_GET_LR()     __builtin_return_address(0)
#define MMPORT_GET_PC(_a)   __asm volatile ("mov %0, pc" : "=r" (_a))
#define MMPORT_MEM_SYNC()   __sync_synchronize()

#endif // MM_HALOW_INCLUDED_MMPORT_H
