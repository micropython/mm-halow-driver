# Host tests for the HaLow allocator. Compiles the REAL src/mm_halow_osal.c
# against stubs, so the tests cannot drift from the code they cover.
#
#   make          build + run
#   make asan     build + run under ASan/UBSan (catches what the checks miss)
HALOW   = ../../src
SDK     = ../../lib/mm-iot-sdk/framework/morselib/include
BUILD   = build
CFLAGS  = -m32 -std=c11 -Wall -Wextra -Werror -g -O1 \
          -I. -I$(HALOW) -I$(SDK) -Wno-unused-parameter
CFLAGS += -MMD -MP

all: run

$(BUILD):
	mkdir -p $@

$(BUILD)/test_alloc: test_alloc.c $(HALOW)/mm_halow_osal.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_alloc.c

run: $(BUILD)/test_alloc
	./$<

$(BUILD)/test_alloc_asan: test_alloc.c $(HALOW)/mm_halow_osal.c | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ test_alloc.c

asan: $(BUILD)/test_alloc_asan
	./$<

clean:
	rm -rf $(BUILD)

-include $(BUILD)/test_alloc.d $(BUILD)/test_alloc_asan.d

.PHONY: all run asan clean
