/*
 * QEMU test that brings the HaLow interface up and down again, running the
 * real driver and morselib against an emulated MM8108 (see mm8108_emu.c).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mm_halow.h"
#include "mm8108_emu.h"

volatile uint32_t mm_halow_test_ticks = 0;

// --- test hooks declared by mm_halow_configport.h ----------------------------
bool mm_halow_test_pin_read(mm_halow_pin_t pin) {
    if (pin == MM_HALOW_IRQ) {
        return mm8108_emu_irq();
    }
    return pin == MM_HALOW_BUSY && mm8108_emu_busy();
}

void mm_halow_test_pin_write(mm_halow_pin_t pin, bool value) {
    if (pin == MM_HALOW_WAKE) {
        mm8108_emu_wake(value);
    } else if (pin == MM_HALOW_RESET && !value) {
        mm8108_emu_reset();
    }
}

void mm_halow_test_event_poll(void) {
    mm_halow_test_ticks += 2;
    if (mm_halow_poll != NULL) {
        mm_halow_poll();
    }
}

// --- port hooks ---------------------------------------------------------------
void mm_halow_port_spi_init(void) {
    printf("spi_init\n");
}

void mm_halow_port_spi_deinit(void) {
    printf("spi_deinit\n");
}

void mm_halow_port_spi_transfer(size_t len, const uint8_t *src, uint8_t *dest) {
    for (size_t i = 0; i < len; i++) {
        uint8_t out = mm8108_emu_spi_byte(src != NULL ? src[i] : 0xff);
        if (dest != NULL) {
            dest[i] = out;
        }
    }
}

uint8_t *mm_halow_port_heap_alloc(size_t size) {
    return calloc(1, size);
}

void mm_halow_port_heap_free(uint8_t *ptr) {
    free(ptr);
}

uint32_t mm_halow_port_random_u32(void) {
    static uint32_t x = 0x12345678;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void mm_halow_port_assert_fail(void) {
    abort();
}

void mm_halow_port_get_mac(uint8_t mac_addr[6]) {
    static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(mac_addr, mac, sizeof(mac));
}

// --- lwIP glue: MM_HALOW_ENABLE_LWIP is off, so mm_halow_lwip.c is empty -----
void mm_halow_cb_tcpip_init(mm_halow_t *self, int itf) {
}

void mm_halow_cb_tcpip_deinit(mm_halow_t *self, int itf) {
}

void mm_halow_cb_tcpip_set_link_up(mm_halow_t *self, int itf) {
}

void mm_halow_cb_tcpip_set_link_down(mm_halow_t *self, int itf) {
}

void mm_halow_cb_process_ethernet(void *cb_data, int itf,
    const uint8_t *header, size_t header_len, const uint8_t *payload, size_t payload_len) {
}

int main(void) {
    int ret = mm_halow_wifi_set_up(&mm_halow_state, MM_HALOW_ITF_STA, true, "US");
    printf("mm_halow_wifi_set_up: %d\n", ret);
    mm_halow_deinit(&mm_halow_state);
    return ret != 0;
}
