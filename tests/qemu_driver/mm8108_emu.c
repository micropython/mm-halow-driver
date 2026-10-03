/*
 * Emulated Morse Micro MM8108, on the far side of the SD-over-SPI bus.
 *
 * Just enough of the transceiver for morselib to load the firmware, boot it
 * and exchange commands with it.  The register addresses and bus behaviour
 * match an SPI trace captured from real hardware; the table layouts are those
 * morselib reads (driver/morse_driver/hw.h, mm8108/yaps-hw.h).
 */

#include <stdio.h>
#include <string.h>

#include "mm8108_emu.h"

#define CHIP_ID             (0x00000809) // MM8108B2
#define REG_CHIP_ID         (0x00002d20)
#define REG_MANIFEST_PTR    (0x00002d40)
#define REG_INT1_STS        (0x00003c50)
#define REG_INT1_CLR        (0x00003c58)
#define REG_INT1_EN         (0x00003c5c)
#define REG_MSI             (0x00004100)
#define HOST_TABLE          (0x0011fd00)
#define EXT_HOST_TABLE      (0x001023c8)
#define YAPS_DATA           (0x00170000) // to-chip packets written, from-chip read
#define YAPS_STATUS         (0x00178000)
#define YAPS_STATUS_LEN     (72)
#define YAPS_POOL_CMD_RESP  (5)
#define SKB_HDR_LEN         (40)
#define CMD_HDR_LEN         (12)
#define RX_SLOTS            (4)
#define CMD_TYPE_RESP       (0x0002)
#define CMD_TYPE_EVT        (0x0004)
#define CMD_GET_VERSION     (0x0002)
#define CMD_ADD_INTERFACE   (0x0004)
#define CMD_HW_SCAN         (0x0044)
#define HW_SCAN_START       (0x0001)
#define HW_SCAN_ABORT       (0x0002)
#define EVT_HW_SCAN_DONE    (0x4011)

static const struct __attribute__((packed)) {
    uint32_t magic, fw_version, host_flags, firmware_flags;
    uint32_t memcmd_cmd_addr, memcmd_resp_addr, ext_host_table_addr;
} host_table = {
    .magic = 0xdeadbeef,
    .fw_version = (57 << 22), // the firmware API major version morselib expects
    .firmware_flags = 0x7fd, // as reported by the real firmware
    .ext_host_table_addr = EXT_HOST_TABLE,
};

static const struct __attribute__((packed)) {
    uint32_t length;
    uint8_t mac[6];
    struct __attribute__((packed)) {
        uint16_t tag, length;
        uint8_t flags, padding[3];
        uint32_t ysl_addr, yds_addr, status_regs_addr;
        uint16_t tc_tx_pool_size, fc_rx_pool_size;
        uint8_t pool_sizes[6]; // cmd, beacon, mgmt, resp, tx_sts, aux
        uint8_t q_sizes[6]; // tx, cmd, beacon, mgmt, fc, fc_done
        uint16_t reserved_page_size, unused;
    } yaps;
} ext_host_table = {
    .length = 4 + 6 + 40,
    .mac = {0x02, 0x4d, 0x4d, 0x00, 0x00, 0x01},
    .yaps = {
        .tag = 3, .length = 40,
        .ysl_addr = YAPS_DATA, .yds_addr = YAPS_DATA, .status_regs_addr = YAPS_STATUS,
        .tc_tx_pool_size = 360, .fc_rx_pool_size = 360,
        .pool_sizes = {16, 8, 8, 8, 8, 8},
        .q_sizes = {16, 4, 4, 4, 16, 16},
    },
};

static struct {
    // SD-over-SPI bus.
    uint8_t cmd[6];
    unsigned cmd_len;
    uint8_t rsp[2];
    unsigned rsp_len;
    uint8_t win[3][2]; // per-function address window, bits 16-31
    enum { BUS_IDLE, BUS_RD_TOKEN, BUS_RD_DATA, BUS_RD_CRC, BUS_WR_TOKEN, BUS_WR_DATA, BUS_WR_CRC } state;
    unsigned fn, blk, blk_left, crc_left;
    uint32_t addr, left;
    uint16_t crc;
    // Chip.
    bool wake;
    uint32_t int1_sts;
    struct {
        uint32_t addr, val;
    } regs[16];
    uint8_t tx[1024]; // packet being written to YAPS
    uint8_t rx[RX_SLOTS][256]; // queued from-chip packets, each delimiter first
    unsigned rx_len[RX_SLOTS], rx_head, rx_count;
} chip;

static uint16_t crc16_xmodem(uint16_t crc, uint8_t b) {
    crc ^= b << 8;
    for (int i = 0; i < 8; i++) {
        crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

static uint8_t crc7_sd(uint8_t crc, uint8_t b) {
    uint8_t x = (crc << 1) ^ b;
    for (int i = 0; i < 8; i++) {
        x = x & 0x80 ? (x << 1) ^ 0x12 : x << 1;
    }
    return x >> 1;
}

static unsigned min(unsigned a, unsigned b) {
    return a < b ? a : b;
}

static uint32_t *chip_reg(uint32_t addr) {
    unsigned i = 0;
    while (chip.regs[i].addr != addr && chip.regs[i].addr != 0) {
        i++;
    }
    chip.regs[i].addr = addr;
    return &chip.regs[i].val;
}

// Queue a message from the chip, a command response or an event, and raise an
// interrupt for it.
static void chip_send(uint16_t flags, uint16_t id, uint16_t host_id, uint16_t vif_id,
    const void *data, unsigned len) {
    uint8_t *rx = chip.rx[(chip.rx_head + chip.rx_count) % RX_SLOTS];
    unsigned msg_len = CMD_HDR_LEN + len;
    unsigned pkt_len = (SKB_HDR_LEN + msg_len + 3) & ~3;
    memset(rx, 0, sizeof(chip.rx[0]));

    // YAPS delimiter: size, pool, CRC7 of the rest.
    uint32_t delim = pkt_len | YAPS_POOL_CMD_RESP << 14;
    uint8_t crc = 0;
    for (int i = 24; i >= 0; i -= 8) {
        crc = crc7_sd(crc, delim >> i);
    }
    delim |= (uint32_t)crc << 25;
    memcpy(rx, &delim, 4);

    rx[4] = 0xaa; // sync
    rx[5] = 0xfe; // command channel
    rx[6] = msg_len;
    rx[7] = msg_len >> 8;
    uint16_t hdr[6] = {flags, id, len, host_id, vif_id, 0};
    memcpy(rx + 4 + SKB_HDR_LEN, hdr, CMD_HDR_LEN);
    memcpy(rx + 4 + SKB_HDR_LEN + CMD_HDR_LEN, data, len);

    chip.rx_len[(chip.rx_head + chip.rx_count) % 4] = 4 + pkt_len;
    chip.rx_count++;
    chip.int1_sts |= 1; // YAPS from-chip packet waiting
}

static void chip_command(const uint8_t *pkt) {
    const uint8_t *cmd = pkt + SKB_HDR_LEN;
    uint16_t id = cmd[2] | cmd[3] << 8;
    uint16_t host_id = cmd[6] | cmd[7] << 8;
    uint16_t vif_id = cmd[8] | cmd[9] << 8;
    printf("chip: command 0x%04x\n", id);

    // Status 0, then a payload of zeroes unless the command needs more.
    uint8_t rsp[4 + 64] = {0};
    if (id == CMD_GET_VERSION) {
        static const char version[] = "rel_mm8108_2_0_0";
        rsp[4] = sizeof(version) - 1;
        memcpy(rsp + 8, version, sizeof(version));
    } else if (id == CMD_ADD_INTERFACE) {
        vif_id = 0; // the id of the new interface
    }
    chip_send(CMD_TYPE_RESP, id, host_id, vif_id, rsp, sizeof(rsp));

    if (id == CMD_HW_SCAN) {
        // Nothing is on the air, so a sweep finishes at once, empty.
        uint32_t flags = cmd[12] | cmd[13] << 8 | cmd[14] << 16 | (uint32_t)cmd[15] << 24;
        if (flags & (HW_SCAN_START | HW_SCAN_ABORT)) {
            uint8_t aborted = (flags & HW_SCAN_ABORT) != 0;
            chip_send(CMD_TYPE_EVT, EVT_HW_SCAN_DONE, 0, 0, &aborted, 1);
        }
    }
}

static uint8_t chip_read(uint32_t addr) {
    uint32_t word = 0;
    if (chip.fn == 1) {
        uint32_t reg = addr & ~3;
        word = reg == REG_CHIP_ID ? CHIP_ID : reg == REG_INT1_STS ? chip.int1_sts : *chip_reg(reg);
    } else if (addr - HOST_TABLE < sizeof(host_table)) {
        return ((const uint8_t *)&host_table)[addr - HOST_TABLE];
    } else if (addr - EXT_HOST_TABLE < sizeof(ext_host_table)) {
        return ((const uint8_t *)&ext_host_table)[addr - EXT_HOST_TABLE];
    } else if (addr - YAPS_STATUS < YAPS_STATUS_LEN) {
        // Free pages for each to-chip pool (all of them: commands are consumed
        // at once), then the count of from-chip packets waiting.
        static const uint32_t pages[4] = {360, 16, 8, 8};
        unsigned i = (addr - YAPS_STATUS) / 4;
        word = i < 4 ? pages[i] : i == 12 ? chip.rx_count : 0;
    } else if (addr - YAPS_DATA < sizeof(chip.rx[0]) && chip.rx_count > 0) {
        // The head packet's delimiter, then its contents; reading the last byte
        // pops it.
        unsigned off = addr - YAPS_DATA;
        uint8_t b = chip.rx[chip.rx_head][off];
        if (off + 1 == chip.rx_len[chip.rx_head]) {
            chip.rx_head = (chip.rx_head + 1) % RX_SLOTS;
            chip.rx_count--;
        }
        return b;
    }
    return word >> (8 * (addr & 3));
}

static void chip_write(uint32_t addr, uint8_t b) {
    if (chip.fn == 1) {
        uint32_t *reg = chip_reg(addr & ~3);
        unsigned shift = 8 * (addr & 3);
        *reg = (*reg & ~(0xffu << shift)) | (uint32_t)b << shift;
        if (shift == 24 && (addr & ~3) == REG_MSI && (*reg & 1)) {
            // The firmware boots and publishes its host table.
            *chip_reg(REG_MANIFEST_PTR) = HOST_TABLE;
        } else if (shift == 24 && (addr & ~3) == REG_INT1_CLR) {
            chip.int1_sts &= ~*reg;
        }
    } else if (addr - YAPS_DATA < sizeof(chip.tx)) {
        // A delimiter then the packet; the firmware is not emulated, so writes
        // anywhere else in memory are dropped.
        unsigned off = addr - YAPS_DATA;
        chip.tx[off] = b;
        if (off >= 3 && off + 1 == 4u + ((chip.tx[0] | chip.tx[1] << 8) & 0x3fff)) {
            chip_command(chip.tx + 4);
        }
    }
}

static void chip_sdio_command(void) {
    uint32_t arg = chip.cmd[1] << 24 | chip.cmd[2] << 16 | chip.cmd[3] << 8 | chip.cmd[4];
    unsigned fn = (arg >> 28) & 7;
    uint32_t addr = (arg >> 9) & 0x1ffff;
    bool write = arg >> 31;
    chip.rsp[0] = 0x00; // R5: no errors
    chip.rsp[1] = 0x00;
    chip.rsp_len = 2;
    if ((chip.cmd[0] & 0x3f) == 52 && write) {
        if (addr - 0x10000 < 2) {
            chip.win[fn][addr - 0x10000] = arg;
        }
        chip.rsp[1] = arg;
    } else if ((chip.cmd[0] & 0x3f) == 53) {
        bool block = (arg >> 27) & 1;
        chip.fn = fn;
        chip.addr = chip.win[fn][1] << 24 | chip.win[fn][0] << 16 | (addr & 0xffff);
        chip.blk = block ? (fn == 1 ? 8 : 512) : (arg & 0x1ff);
        chip.left = block ? (arg & 0x1ff) * chip.blk : chip.blk;
        chip.state = write ? BUS_WR_TOKEN : BUS_RD_TOKEN;
    }
}

uint8_t mm8108_emu_spi_byte(uint8_t in) {
    if (chip.rsp_len > 0) {
        chip.rsp_len--;
        return chip.rsp[1 - chip.rsp_len];
    }
    uint8_t out = 0xff;
    switch (chip.state) {
        case BUS_IDLE:
            if (chip.cmd_len > 0 || (in & 0xc0) == 0x40) {
                chip.cmd[chip.cmd_len++] = in;
                if (chip.cmd_len == 6) {
                    chip.cmd_len = 0;
                    chip_sdio_command();
                }
            }
            break;
        case BUS_RD_TOKEN:
            out = 0xfe;
            chip.blk_left = min(chip.blk, chip.left);
            chip.crc = 0;
            chip.state = BUS_RD_DATA;
            break;
        case BUS_RD_DATA:
            out = chip_read(chip.addr++);
            chip.crc = crc16_xmodem(chip.crc, out);
            chip.left--;
            if (--chip.blk_left == 0) {
                chip.crc_left = 2;
                chip.state = BUS_RD_CRC;
            }
            break;
        case BUS_RD_CRC:
            out = chip.crc >> (8 * --chip.crc_left);
            if (chip.crc_left == 0) {
                chip.state = chip.left ? BUS_RD_TOKEN : BUS_IDLE;
            }
            break;
        case BUS_WR_TOKEN:
            if (in == 0xfe || in == 0xfc) {
                chip.blk_left = min(chip.blk, chip.left);
                chip.state = BUS_WR_DATA;
            }
            break;
        case BUS_WR_DATA:
            chip_write(chip.addr++, in);
            chip.left--;
            if (--chip.blk_left == 0) {
                chip.crc_left = 2;
                chip.state = BUS_WR_CRC;
            }
            break;
        case BUS_WR_CRC:
            if (--chip.crc_left == 0) {
                chip.rsp[1] = 0xe5; // data accepted
                chip.rsp_len = 1;
                chip.state = chip.left ? BUS_WR_TOKEN : BUS_IDLE;
            }
            break;
    }
    return out;
}

bool mm8108_emu_irq(void) {
    return (chip.int1_sts & *chip_reg(REG_INT1_EN)) == 0;
}

bool mm8108_emu_busy(void) {
    return chip.wake;
}

void mm8108_emu_wake(bool wake) {
    chip.wake = wake;
}

void mm8108_emu_reset(void) {
    memset(&chip, 0, sizeof(chip));
}
