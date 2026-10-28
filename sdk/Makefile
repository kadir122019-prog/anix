SDK_CC ?= gcc
SDK_CFLAGS ?= -std=gnu11 -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -fno-pic -m64 -march=x86-64 -mno-red-zone -mcmodel=small -Wall -Wextra -Werror
SDK_LDFLAGS ?= -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,sdk/linker.ld
SDK_APP_SRC ?= sdk/examples/hello.c
SDK_OUTPUT ?= build/sdk/$(basename $(notdir $(firstword $(SDK_APP_SRC)))).elf
SDK_EXTRA_CFLAGS ?=
SDK_EXTRA_LDFLAGS ?=
SDK_EXTRA_DEPS ?=
SDK_HEADERS := $(shell find sdk/include include/anix -type f -name '*.h' 2>/dev/null)
ifeq ($(ANIX_ENABLE_STDIN_TEST),1)
SDK_CFLAGS += -DANIX_ENABLE_STDIN_TEST=1
endif

.PHONY: all clean
all: $(SDK_OUTPUT)


$(SDK_OUTPUT): sdk/crt0.S $(SDK_APP_SRC) sdk/lib/string.c sdk/lib/heap.c sdk/lib/stdio.c sdk/linker.ld $(SDK_HEADERS) $(SDK_EXTRA_DEPS)
	@mkdir -p $(@D)
	$(SDK_CC) $(SDK_CFLAGS) $(SDK_EXTRA_CFLAGS) -Isdk/include -Iinclude $(SDK_LDFLAGS) $(SDK_EXTRA_LDFLAGS) sdk/crt0.S $(SDK_APP_SRC) sdk/lib/string.c sdk/lib/heap.c sdk/lib/stdio.c -o $@

clean:
	rm -f $(SDK_OUTPUT)
