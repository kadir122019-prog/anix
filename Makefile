PROJECT := anix
BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso_root
ISO := $(BUILD_DIR)/anix.iso
KERNEL := $(BUILD_DIR)/anix.elf
INITRAMFS := $(BUILD_DIR)/initramfs.cpio
TOOLCHAIN_PREFIX ?= x86_64-elf-
RUSTC ?= rustc
ANIX_ENABLE_TRANSITION_TEST ?= 0
ANIX_ENABLE_STORAGE_TEST ?= 0
ANIX_ENABLE_STDIN_TEST ?= 0
ANIX_SHELL_BOOT ?= 0
ANIX_ENABLE_DESKTOP ?= 0
ifeq ($(shell command -v $(TOOLCHAIN_PREFIX)gcc 2>/dev/null),)
CC := gcc
else
CC := $(TOOLCHAIN_PREFIX)gcc
endif
CFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto -fno-pie -fno-pic -m64 -march=x86-64 -mgeneral-regs-only -mno-red-zone -mcmodel=kernel -Wall -Wextra -Wpedantic -Werror -Iinclude -Ithird_party/limine
ifeq ($(ANIX_ENABLE_TRANSITION_TEST),1)
CFLAGS += -DANIX_ENABLE_TRANSITION_TEST=1
endif
ifeq ($(ANIX_ENABLE_DESKTOP),1)
CFLAGS += -DANIX_ENABLE_DESKTOP=1
endif
ifeq ($(ANIX_ENABLE_STORAGE_TEST),1)
CFLAGS += -DANIX_ENABLE_STORAGE_TEST=1
endif
LDFLAGS := -nostdlib -static -no-pie -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -Tarch/x86_64/linker.ld
SOURCES := $(shell find boot kernel mm interrupts drivers lib arch/x86_64 sched syscall vfs fs -name '*.c' -o -name '*.S' 2>/dev/null) userspace/elf.c userspace/file.c userspace/enter_user.S
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(filter %.c,$(SOURCES))) $(patsubst %.S,$(BUILD_DIR)/%.o,$(filter %.S,$(SOURCES)))
RUST_OBJECT := $(BUILD_DIR)/rust/process_group.a
DEPS := $(OBJECTS:.o=.d)
CONFIG_STAMP := $(BUILD_DIR)/config-transition-$(ANIX_ENABLE_TRANSITION_TEST)-storage-$(ANIX_ENABLE_STORAGE_TEST)-stdin-$(ANIX_ENABLE_STDIN_TEST)-shell-$(ANIX_SHELL_BOOT)-desktop-$(ANIX_ENABLE_DESKTOP).stamp
.PHONY: all kernel iso iso-shell iso-desktop run debug test test-transition test-storage test-stdin clean distclean check-deps help limine sdk-demo sdk-program sdk-install apm-test apm-build apm-sample
all: kernel
kernel: $(KERNEL)
$(KERNEL): $(OBJECTS) $(RUST_OBJECT) arch/x86_64/linker.ld
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(OBJECTS) $(RUST_OBJECT) $(LDFLAGS) -o $@

$(RUST_OBJECT): kernel/rust/process_group.rs
	@mkdir -p $(@D)
	$(RUSTC) --edition=2021 --crate-name anix_process_group --crate-type staticlib --target x86_64-unknown-none -C panic=abort -C opt-level=2 $< -o $@
	@printf 'Built %s\n' '$@'
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(CONFIG_STAMP):
	@mkdir -p $(@D)
	@rm -f $(BUILD_DIR)/config-*.stamp
	@touch $@
$(OBJECTS): $(CONFIG_STAMP)
limine:
	./scripts/fetch-limine.sh
iso: check-deps kernel limine $(INITRAMFS)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/anix.elf
	cp $(INITRAMFS) $(ISO_ROOT)/boot/initramfs.cpio
	cp limine.conf $(ISO_ROOT)/limine.conf
	cp third_party/limine/bin/limine-bios.sys third_party/limine/bin/limine-bios-cd.bin third_party/limine/bin/limine-uefi-cd.bin $(ISO_ROOT)/
	cp third_party/limine/bin/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label $(ISO_ROOT) -o $(ISO)
	third_party/limine/bin/limine bios-install $(ISO)
	@test -s $(ISO)
iso-shell:
	$(MAKE) ANIX_SHELL_BOOT=1 ISO=$(BUILD_DIR)/anix-shell.iso iso
iso-desktop:
	$(MAKE) ANIX_ENABLE_DESKTOP=1 ANIX_SHELL_BOOT=1 ISO=$(BUILD_DIR)/anix-desktop.iso iso
USER_CFLAGS := -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-pie -fno-pic -m64 -mno-red-zone -mcmodel=small -Wall -Wextra -Werror
ifeq ($(ANIX_SHELL_BOOT),1)
USER_CFLAGS += -DANIX_SHELL_BOOT=1
endif
ifeq ($(ANIX_ENABLE_STDIN_TEST),1)
USER_CFLAGS += -DANIX_ENABLE_STDIN_TEST=1
endif
USER_INIT := $(BUILD_DIR)/user-init.elf
USER_SHELL := $(BUILD_DIR)/anixsh.elf
SDK_APP := $(BUILD_DIR)/sdk/hello.elf
SDK_WORKER := $(BUILD_DIR)/sdk/worker.elf
SDK_ORPHAN := $(BUILD_DIR)/sdk/orphan.elf
SDK_PIPE_READER := $(BUILD_DIR)/sdk/pipe-reader.elf
USER_ROOT := $(BUILD_DIR)/initramfs-root
USER_ROOT_STAMP := $(BUILD_DIR)/initramfs-root.stamp
sdk-demo: $(SDK_APP)
ANIX_SDK_SRC ?=
ANIX_SDK_NAME ?= app
ANIX_SDK_OUTPUT ?= $(BUILD_DIR)/sdk/$(ANIX_SDK_NAME).elf
sdk-program:
	@test -n "$(ANIX_SDK_SRC)" || { printf '%s\n' 'Set ANIX_SDK_SRC to your C source file(s).'; exit 2; }
	$(MAKE) -f sdk/Makefile SDK_APP_SRC="$(ANIX_SDK_SRC)" SDK_OUTPUT="$(ANIX_SDK_OUTPUT)"
sdk-install: sdk-program
	@test "$(notdir $(ANIX_SDK_NAME))" = "$(ANIX_SDK_NAME)" && test "$(ANIX_SDK_NAME)" != . && test "$(ANIX_SDK_NAME)" != .. || { printf '%s\n' 'ANIX_SDK_NAME must be a simple file name.'; exit 2; }
	$(MAKE) ANIX_SHELL_BOOT=1 $(USER_ROOT_STAMP)
	mkdir -p $(USER_ROOT)/bin
	cp $(ANIX_SDK_OUTPUT) $(USER_ROOT)/bin/$(ANIX_SDK_NAME)
	python3 scripts/mkinitramfs.py $(USER_ROOT) $(INITRAMFS)
	$(MAKE) ANIX_SHELL_BOOT=1 ISO=$(BUILD_DIR)/anix-sdk.iso iso
apm-test:
	CARGO_PROFILE_TEST_DEBUG=0 CARGO_INCREMENTAL=0 cargo test -j 1 --manifest-path apm/Cargo.toml
apm-build:
	CARGO_PROFILE_RELEASE_DEBUG=0 CARGO_INCREMENTAL=0 cargo build --release -j 1 --manifest-path apm/Cargo.toml
apm-sample:
	sh apm/scripts/build-sample.sh
$(SDK_APP): sdk/crt0.S sdk/examples/hello.c sdk/lib/string.c sdk/lib/heap.c sdk/lib/stdio.c sdk/linker.ld sdk/include/anix/syscall.h sdk/include/anix/libc.h sdk/include/anix/stdlib.h sdk/include/anix/stdio.h include/anix/syscall_abi.h include/anix/stat.h sdk/Makefile $(CONFIG_STAMP)
	$(MAKE) -f sdk/Makefile SDK_OUTPUT=$(SDK_APP) ANIX_ENABLE_STDIN_TEST=$(ANIX_ENABLE_STDIN_TEST)
$(SDK_WORKER): sdk/crt0.S sdk/examples/worker.c sdk/lib/string.c sdk/lib/heap.c sdk/lib/stdio.c sdk/linker.ld sdk/include/anix/syscall.h sdk/include/anix/libc.h sdk/include/anix/stdlib.h sdk/include/anix/stdio.h include/anix/syscall_abi.h include/anix/stat.h sdk/Makefile
	$(MAKE) -f sdk/Makefile SDK_OUTPUT=$(SDK_WORKER) SDK_APP_SRC=sdk/examples/worker.c ANIX_ENABLE_STDIN_TEST=$(ANIX_ENABLE_STDIN_TEST)
$(SDK_ORPHAN): sdk/crt0.S sdk/examples/orphan.c sdk/lib/string.c sdk/lib/heap.c sdk/lib/stdio.c sdk/linker.ld sdk/include/anix/syscall.h sdk/include/anix/libc.h sdk/include/anix/stdlib.h sdk/include/anix/stdio.h include/anix/syscall_abi.h include/anix/stat.h sdk/Makefile
	$(MAKE) -f sdk/Makefile SDK_OUTPUT=$(SDK_ORPHAN) SDK_APP_SRC=sdk/examples/orphan.c
$(SDK_PIPE_READER): sdk/crt0.S sdk/examples/pipe-reader.c sdk/lib/string.c sdk/lib/heap.c sdk/lib/stdio.c sdk/linker.ld sdk/include/anix/syscall.h sdk/include/anix/libc.h include/anix/syscall_abi.h sdk/Makefile
	$(MAKE) -f sdk/Makefile SDK_OUTPUT=$(SDK_PIPE_READER) SDK_APP_SRC=sdk/examples/pipe-reader.c
$(USER_INIT): userspace/programs/init.c userspace/userabi.h userspace/start.S userspace/linker.ld $(CONFIG_STAMP)
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,--defsym=USER_BASE=0x400000 -Wl,-T,userspace/linker.ld userspace/start.S userspace/programs/init.c -o $@
$(USER_SHELL): userspace/programs/anixsh.c userspace/userabi.h userspace/start.S userspace/linker.ld
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,--defsym=USER_BASE=0x500000 -Wl,-T,userspace/linker.ld userspace/start.S userspace/programs/anixsh.c -o $@
$(USER_ROOT_STAMP): $(shell find userspace/initramfs -type f) $(USER_INIT) $(USER_SHELL) $(SDK_APP) $(SDK_WORKER) $(SDK_ORPHAN) $(SDK_PIPE_READER)
	rm -rf $(USER_ROOT)
	cp -R userspace/initramfs $(USER_ROOT)
	mkdir -p $(USER_ROOT)/bin
	cp $(USER_INIT) $(USER_ROOT)/bin/init
	cp $(USER_SHELL) $(USER_ROOT)/bin/anixsh
	cp $(SDK_APP) $(USER_ROOT)/bin/sdk-hello
	cp $(SDK_WORKER) $(USER_ROOT)/bin/sdk-worker
	cp $(SDK_ORPHAN) $(USER_ROOT)/bin/sdk-orphan
	cp $(SDK_PIPE_READER) $(USER_ROOT)/bin/sdk-pipe-reader
	touch $@
$(INITRAMFS): $(USER_ROOT_STAMP) scripts/mkinitramfs.py
	@mkdir -p $(@D)
	python3 scripts/mkinitramfs.py $(USER_ROOT) $@
run: iso
	./scripts/run.sh --no-build
debug: iso
	./scripts/run.sh --no-build --debug
test: kernel
	@if [ "$(ANIX_ENABLE_TRANSITION_TEST)" = 1 ]; then ANIX_TRANSITION_TEST=1 ./scripts/test.sh --no-build; else ./scripts/test.sh --no-build; fi
test-transition:
	$(MAKE) clean
	$(MAKE) ANIX_ENABLE_TRANSITION_TEST=1 test
test-storage:
	$(MAKE) clean
	$(MAKE) ANIX_ENABLE_STORAGE_TEST=1 iso
	python3 scripts/test-storage.py
test-stdin:
	$(MAKE) clean
	$(MAKE) ANIX_ENABLE_STDIN_TEST=1 iso
	python3 scripts/test-userspace.py --stdin-test
	$(MAKE) clean
	$(MAKE) iso
check-deps:
	@RUSTC=$(RUSTC) ./scripts/check-deps.sh all
clean:
	rm -rf $(BUILD_DIR)
distclean: clean
	rm -rf third_party/limine/bin
help:
	@printf '%s\n' 'Anix targets:' '  all/kernel  Build the freestanding kernel ELF' '  sdk-demo    Build the separate freestanding userspace SDK example' '  sdk-program Build a custom SDK app (ANIX_SDK_SRC, ANIX_SDK_NAME)' '  sdk-install Build and package a custom app into build/anix-sdk.iso' '  iso         Build the BIOS/UEFI SDK test-image ISO' '  iso-shell   Build build/anix-shell.iso and boot directly to anixsh' '  iso-desktop Build build/anix-desktop.iso with opt-in pointer/input' '  run         Boot ISO in QEMU' '  debug       Boot QEMU paused with GDB server on :1234' '  test        Run host tests and QEMU smoke test when available' '  test-transition Clean build, enable the CR3 round-trip, and require QEMU success' '  test-storage Clean build, enable ATA test hooks, verify sector I/O and flushed image data' '  test-stdin  Verify ring-3 read(0) line input then restore the default ISO' '  clean       Remove build outputs' '  distclean   Also remove downloaded Limine binaries' '  check-deps  Report tool availability'
-include $(DEPS)
