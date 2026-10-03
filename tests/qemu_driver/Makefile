# QEMU test that brings the whole driver up and down, linked against the real
# morselib, on a Cortex-M55 (mps3-an547 == the N6's core).  The transceiver is
# emulated in mm8108_emu.c.
#
#   make        build + run under QEMU
#
# Cortex-M55 needs arm-none-eabi-gcc 14 or newer.  Override CROSS_COMPILE to
# point at a suitable toolchain if the default is too old.
CROSS_COMPILE ?= arm-none-eabi-
CC       = $(CROSS_COMPILE)gcc
OBJCOPY  = $(CROSS_COMPILE)objcopy

TOP      = ../..
BUILD    = build
PROG     = $(BUILD)/test_bringup.elf

# Hard float, as the N6 is built, so the scheduler's vpush {d8-d15} path is live.
ARCH     = -mthumb -mcpu=cortex-m55 -mfloat-abi=hard -mfpu=fpv5-d16
CFLAGS   = $(ARCH) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Og -g \
           -ffreestanding -fno-common -MMD -MP \
           -I. -I$(TOP)/src -I$(MM_HALOW_MORSELIB_DIR)/include \
           -I$(MM_HALOW_MMIOT_DIR)/src/mmregdb -I$(MM_HALOW_MMIOT_DIR)/src/mmutils
LDFLAGS  = $(ARCH) -Tmps3.ld -nostartfiles -specs=nosys.specs -Wl,--gc-sections \
           -Wl,-Map=$(PROG:.elf=.map) -Wl,--no-warn-rwx-segments

all: run

# Adds morselib to LIBS and the firmware/BCF blobs to OBJ.
MM_HALOW_TOP = $(TOP)
Q     = @
ECHO  = @echo
MKDIR = mkdir
include $(TOP)/mm_halow.mk

SRC = \
	startup.c \
	syscalls.c \
	test_bringup.c \
	mm8108_emu.c \
	$(addprefix $(TOP)/src/,\
		mm_halow_ctrl.c \
		mm_halow_hal.c \
		mm_halow_lwip.c \
		mm_halow_osal.c \
		mm_halow_pktmem.c \
		mm_halow_sched.c \
		) \
	$(addprefix $(MM_HALOW_MMIOT_DIR)/src/,\
		mmregdb/mmregdb.c \
		mmutils/mmutils_wlan.c \
		)

# Each object mirrors its source's path under $(BUILD), with files from the
# rest of the repository placed relative to $(TOP): src/foo.c builds to
# $(BUILD)/src/foo.o.
OBJ += $(addprefix $(BUILD)/,$(patsubst $(TOP)/%,%,$(SRC:.c=.o)))

$(BUILD)/%.o: %.c
	$(ECHO) "CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(TOP)/%.c
	$(ECHO) "CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(PROG): $(OBJ) mps3.ld
	$(ECHO) "LD $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

run: $(PROG)
	@qemu-system-arm -M mps3-an547 -cpu cortex-m55 -nographic \
	    -semihosting-config enable=on,target=native -kernel $<

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)

.PHONY: all run clean
