# QEMU tests for mm_halow_sched.c on a Cortex-M55 (mps3-an547 == the N6's core).
#
#   make                    build + run under QEMU with GCC
#   make CC=clang GCC=...    build with clang, link with the GCC toolchain
#
# Cortex-M55 needs arm-none-eabi-gcc 14 or newer (Ubuntu 24.04 ships 13.2,
# which rejects -mcpu=cortex-m55 with hard float).  Override CC to point at a
# suitable toolchain if the default is too old.
CC       = arm-none-eabi-gcc

# GCC toolchain used to link (it resolves newlib and the target multilib via
# -specs, which clang cannot consume) and, for a clang build, to supply the
# bare-metal sysroot.  Point it at a GCC >= 14 if the default is too old.
GCC      = arm-none-eabi-gcc

TOP      = ../..
HALOW    = $(TOP)/src
# mps3.ld is a copy of MicroPython's ports/qemu/mcu/arm/mps3.ld (MIT).
LDSCRIPT = mps3.ld
BUILD    = build

# Matches ports/qemu/boards/MPS3_AN547/mpconfigboard.mk, which in turn matches
# how the N6 is built: hard float, so the vpush {d8-d15} path is live.
ARCH   = -mthumb -mcpu=cortex-m55 -mfloat-abi=hard -mfpu=fpv5-d16
CFLAGS = $(ARCH) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Og -g \
          -ffreestanding -fno-common \
          -I. -I$(HALOW) -I../../lib/mm-iot-sdk/framework/morselib/include
LDFLAGS = $(ARCH) -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/test_sched.map -specs=nosys.specs \
           -Wl,--no-warn-rwx-segments

# clang can cross-compile the sources for the Cortex-M55, but it needs the
# bare-metal sysroot (headers + newlib) from the GCC toolchain, its Thumb frame
# pointer (r7) freed so the register-preservation test can clobber it, and the
# ARM EABI short-enum ABI to match newlib.  It cannot consume GCC's -specs, so
# the link is done with GCC either way.
LD = $(CC)
ifneq ($(findstring clang,$(CC)),)
CFLAGS += --target=arm-none-eabi --sysroot=$(shell $(GCC) -print-sysroot) \
          -fomit-frame-pointer -fshort-enums
LDFLAGS += -Wl,--no-warn-execstack
LD = $(GCC)
endif

SRC += startup.c test_sched.c $(HALOW)/mm_halow_sched.c

# Each object mirrors its source's path under $(BUILD), with files from the
# rest of the repository placed relative to $(TOP): src/foo.c builds to
# $(BUILD)/src/foo.o.  Sources that share a name in different directories
# therefore get distinct objects.
OBJ += $(addprefix $(BUILD)/,$(patsubst $(TOP)/%,%,$(SRC:.c=.o)))

all: run

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(TOP)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_sched.elf: $(OBJ) $(LDSCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

run: $(BUILD)/test_sched.elf
	@qemu-system-arm -M mps3-an547 -cpu cortex-m55 -nographic \
	    -semihosting-config enable=on,target=native \
	    -kernel $<

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
