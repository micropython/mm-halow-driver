Morse Micro 802.11ah HaLow driver
=================================

A portable driver for the Morse Micro MM6108/MM8108 802.11ah (Wi-Fi HaLow)
transceivers: host-agnostic C that an embedding environment integrates by
providing a small configuration header.  It presents a compact station model --
a state object holding the lwIP interfaces, a link status that folds the WLAN
and TCP/IP state together, and a poll function the host drives.

The vendor stack (`morselib`) and the transceiver firmware/board-configuration
blobs come from the Morse Micro
[MM-IoT-SDK](https://github.com/MorseMicro/mm-iot-sdk), included here as the
`lib/mm-iot-sdk` submodule.  `morselib` is a prebuilt library under the Morse
Micro Binary Distribution Licence; the driver runs it cooperatively off the
host's scheduler through a small OSAL shim -- no vendor RTOS.

## Layout

- `src/` -- the driver:
  - `mm_halow_config.h` -- the host integration contract.  The embedder
    provides `mm_halow_configport.h` (or the file named by
    `MM_HALOW_CONFIG_FILE`) supplying atomic sections, a millisecond tick,
    pin accessors, and the `mm_halow_port_*` functions (SPI transport, private
    heap backing, hardware RNG, fallback MAC, optional edge IRQ).
  - `mm_halow_ctrl.c` -- driver core: init/deinit, connect/scan/status, the
    morselib event plumbing.
  - `mm_halow_lwip.c` -- the lwIP netif.
  - `mm_halow_hal.c` -- morselib's `mmhal` interface, mapped onto the
    `mm_halow_port_*` hooks (SD-over-SPI framing, reset/wake sequencing,
    firmware/BCF blob serving).
  - `mm_halow_osal.c` / `mm_halow_sched.c` -- morselib's `mmosal` interface: a
    private heap (morselib allocates from interrupt context, where a host GC
    must not run) and a cooperative scheduler with a wall-clock budget.
  - `mm_halow_pktmem.c` -- the packet-memory pool.
  - `mmport.h` -- morselib's port header (name fixed by the SDK).
- `mm_halow.mk` -- build fragment for make-based embedders: prebuilt morselib
  linkage (grouped with the target multilib's libc/libm) and the firmware/BCF
  blob embedding.
- `tests/host` -- allocator tests, compiled natively from the real sources
  (`make`, `make asan`).
- `tests/qemu` -- scheduler tests on a Cortex-M55 under QEMU (`make`), since
  the context switch is naked asm that cannot run on the host.

## Integrating

1. Add `src/` to the include path along with
   `lib/mm-iot-sdk/framework/morselib/include`, and compile `src/*.c`.
2. Provide `mm_halow_configport.h`; `src/mm_halow_config.h` documents every
   hook and fails the build naming whatever is missing.
3. Link the prebuilt `morselib` and the transceiver firmware blob --
   `mm_halow.mk` does both for make-based builds.  morselib is built against
   newlib and pulls in a few C library functions (`sscanf`, `qsort`, `setjmp`,
   ...) that in turn reference newlib's syscall back-end (`_sbrk`, `_write`,
   ...).  The host must provide that back-end; MicroPython's ports already do.
   morselib never calls these at run time, so the back-end only needs to
   satisfy the link -- and `_sbrk` in particular should fail rather than hand
   out memory, since the driver runs entirely from its own heap.  (If the host
   links `-nostdlib`, also group libc/libm with morselib, as the fragment
   does.)
4. Drive `mm_halow_poll()` from the host's deferred-work mechanism and call
   `mm_halow_schedule_poll()` when the transceiver's IRQ line asserts (or poll
   by level; the driver checks the line each pass).

The MicroPython `network.HALOW` binding is the reference embedding.
