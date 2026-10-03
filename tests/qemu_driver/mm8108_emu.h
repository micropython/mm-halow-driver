// Emulated Morse Micro MM8108 transceiver, see mm8108_emu.c.
#ifndef MM8108_EMU_H
#define MM8108_EMU_H

#include <stdbool.h>
#include <stdint.h>

// One byte each way on the SPI bus: MOSI in, MISO returned.
uint8_t mm8108_emu_spi_byte(uint8_t in);

// IRQ line: active low, while an enabled interrupt is pending.
bool mm8108_emu_irq(void);

// BUSY line: the transceiver is awake for as long as WAKE asks it to be.
bool mm8108_emu_busy(void);

// Let time pass for the transceiver.
void mm8108_emu_tick(unsigned ms);

// WAKE and RESET lines.
void mm8108_emu_wake(bool wake);
void mm8108_emu_reset(void);

#endif
