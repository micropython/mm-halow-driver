# Build fragment for embedding builds (make-based): links the prebuilt
# morselib and the transceiver firmware/BCF blobs.  The including build must
# provide TOP, BUILD, CC, CFLAGS, OBJCOPY and collect LIBS/OBJ/CFLAGS.

MM_HALOW_DIR ?= lib/mm-halow-driver
MM_HALOW_TOP ?= $(TOP)/$(MM_HALOW_DIR)
MM_HALOW_MMIOT_DIR = $(MM_HALOW_TOP)/lib/mm-iot-sdk/framework
MM_HALOW_MORSELIB_DIR = $(MM_HALOW_MMIOT_DIR)/morselib

# morselib is distributed as a prebuilt library under the Morse Micro Binary
# Distribution Licence.  Building it from source instead is useful for
# debugging, but those sources are GPL-3.0, which is not compatible with the
# rest of this firmware, so the prebuilt library is the default.
MM_HALOW_MORSELIB_CORE ?= arm-cortex-m33f

# The MM-IoT-SDK (prebuilt morselib + the transceiver firmware blobs) is a
# submodule of this driver, i.e. a submodule of a submodule of the embedding
# project.  A recursive checkout populates it, but the embedder's flat "fetch
# my submodules" step is usually not recursive (MicroPython's `make submodules`
# is not), which leaves the SDK empty -- and both its source files (compiled
# below) and its firmware blobs then fail with "No such file".  Fetch it here
# at parse time, before anything reads from it, guarded so it is a no-op once
# the SDK is present.
ifeq ($(wildcard $(MM_HALOW_MORSELIB_DIR)/lib/$(MM_HALOW_MORSELIB_CORE)/libmorse.a),)
$(info Fetching the mm-iot-sdk nested submodule...)
$(shell cd $(MM_HALOW_TOP) && git submodule update --init lib/mm-iot-sdk >&2)
endif
ifeq ($(MM_HALOW_MORSELIB_SOURCE),1)
INC += $(addprefix -I$(MM_HALOW_MORSELIB_DIR)/,src src/internal src/emmet src/umac/rc/mmrc_osal mmrc/src/core)
SRC_THIRDPARTY_C += $(patsubst $(TOP)/%,%,\
	$(shell find $(MM_HALOW_MORSELIB_DIR)/src $(MM_HALOW_MORSELIB_DIR)/mmrc/src -name '*.c'))
CFLAGS_THIRDPARTY += -DLOOKAROUND_FAIL_MAX=50 -Wno-c++-compat
else
# morselib is prebuilt against newlib, but the ports link with -nostdlib, so the
# C library functions it calls (sscanf, qsort, setjmp, _ctype_, ...) are not
# otherwise pulled in.  Resolve libc/libm for the target multilib the same way
# the ports resolve libgcc, and group them with the archive so the linker settles
# the references between morselib and libc regardless of order.  These are lazily
# expanded: CFLAGS only carries the -mcpu flags that select the multilib once the
# including port has finished adding them, after this file is included.
MM_HALOW_LIBC = $(shell $(CC) $(CFLAGS) -print-file-name=libc.a)
MM_HALOW_LIBM = $(shell $(CC) $(CFLAGS) -print-file-name=libm.a)
LIBS += -Wl,--start-group $(MM_HALOW_MORSELIB_DIR)/lib/$(MM_HALOW_MORSELIB_CORE)/libmorse.a $(MM_HALOW_LIBC) $(MM_HALOW_LIBM) -Wl,--end-group
endif

# objcopy derives a blob's symbol names from its path, mangling everything that
# is not alphanumeric into an underscore.
mm_halow_blob_sym = _binary_$(subst .,_,$(subst -,_,$(subst /,_,$(1))))_$(2)

# Output format for the blobs.  Overridable, as the only thing tying the driver
# to a particular architecture is the prebuilt morselib.
MM_HALOW_BFDNAME ?= elf32-littlearm
MM_HALOW_BFDARCH ?= arm

# The transceiver firmware, and optionally a board configuration file holding
# calibration data, are linked in as binary blobs.  A board picks its BCF by
# name with MM_HALOW_BCF; the SDK keeps them per chip under morsefirmware.
MM_HALOW_CHIPSET ?= mm8108
MM_HALOW_BCF ?= mf15457
MM_HALOW_FW_MBIN ?= $(MM_HALOW_MMIOT_DIR)/morsefirmware/mm8108b2-rl.mbin
ifneq ($(MM_HALOW_BCF),)
MM_HALOW_BCF_MBIN ?= $(MM_HALOW_MMIOT_DIR)/morsefirmware/$(MM_HALOW_CHIPSET)/bcfs/bcf_$(MM_HALOW_BCF).mbin
endif
MM_HALOW_FW_OBJ = $(BUILD)/$(MM_HALOW_DIR)/mm_halow_firmware.o

$(MM_HALOW_FW_OBJ): $(MM_HALOW_FW_MBIN)
	$(ECHO) "GEN $@"
	$(Q)$(MKDIR) -p $(dir $@)
	$(Q)$(OBJCOPY) -I binary -O $(MM_HALOW_BFDNAME) -B $(MM_HALOW_BFDARCH) $< $@ \
		--redefine-sym $(call mm_halow_blob_sym,$<,start)=mm_halow_firmware_start \
		--redefine-sym $(call mm_halow_blob_sym,$<,end)=mm_halow_firmware_end \
		--rename-section .data=.rodata.mm_halow_firmware,contents,alloc,load,readonly,data \
		--set-section-alignment .data=4

ifneq ($(MM_HALOW_BCF_MBIN),)
CFLAGS += -DMM_HALOW_BCF=1
MM_HALOW_BCF_OBJ = $(BUILD)/$(MM_HALOW_DIR)/mm_halow_bcf.o

$(MM_HALOW_BCF_OBJ): $(MM_HALOW_BCF_MBIN)
	$(ECHO) "GEN $@"
	$(Q)$(MKDIR) -p $(dir $@)
	$(Q)$(OBJCOPY) -I binary -O $(MM_HALOW_BFDNAME) -B $(MM_HALOW_BFDARCH) $< $@ \
		--redefine-sym $(call mm_halow_blob_sym,$<,start)=mm_halow_bcf_start \
		--redefine-sym $(call mm_halow_blob_sym,$<,end)=mm_halow_bcf_end \
		--rename-section .data=.rodata.mm_halow_bcf,contents,alloc,load,readonly,data \
		--set-section-alignment .data=4
endif

OBJ += $(MM_HALOW_FW_OBJ) $(MM_HALOW_BCF_OBJ)
