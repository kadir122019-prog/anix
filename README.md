# Anix

Anix is a modular x86-64 kernel project with a separate freestanding userspace SDK for developing and exercising the kernel ABI. The included initramfs programs are development fixtures for kernel testing.

## Current milestone

The kernel boots a kernel-owned address space, initializes PMM/VMM/KVA/heap, discovers ACPI interrupt controllers and S5 poweroff controls, services APIC timer and PS/2 keyboard interrupts, and mounts a CPIO initramfs plus an optional disk-backed AnixFS volume at `/persist`. A single-core round-robin scheduler preempts tasks on APIC timer interrupts and supports explicit yields. The QEMU test image starts a small ring-3 harness that exercises memory, process, syscall, pipe, and VFS interfaces before launching the `/bin/anixsh` shell.

The kernel also retains the opt-in CR3 round-trip verification path. `make test-transition` checks the candidate-root continuation and rollback under QEMU. See [docs/STATUS.md](docs/STATUS.md) for implementation details and current kernel scope.

The display uses the firmware's GOP/Limine framebuffer and includes a software text console. The shell build keeps mouse hardware and its pointer disabled; `make iso-desktop` builds a separate shell image with mouse input and the pointer enabled for desktop development. This flag does not provide a window manager. PS/2 keyboard and mouse events share a blocking SDK event API. USB HID/xHCI and native GPU acceleration are not implemented yet.

## Dependencies

Required for kernel compilation: GNU Make, GCC/binutils capable of x86-64 freestanding output, and Rust with the `x86_64-unknown-none` target. `x86_64-elf-gcc` is selected when present; host GCC is a fallback.

Required for ISO and execution: curl or wget, xorriso, and QEMU (`qemu-system-x86_64`). The ISO build downloads the Limine `v12.9.4` binary release, builds its host utility, and copies the required runtime artifacts into `third_party/limine/bin/`. Set `LIMINE_VERSION` to use another compatible release.

## Build and run

```sh
make                 # kernel ELF only
make test            # host tests; QEMU smoke test if dependencies exist
make test-transition # explicitly opt-in CR3 switch, continuation checks, and rollback under QEMU
./scripts/build.sh    # QEMU kernel test ISO
./scripts/run.sh      # QEMU, serial console on the terminal
./scripts/run.sh --debug
```

For a direct shell boot, build `build/anix-shell.iso` with `make iso-shell`; the default `make iso` image retains the SDK test harness. The VMware Player profile uses the shell-first image: open [vmware/Anix.vmx](vmware/Anix.vmx). See [vmware/README.md](vmware/README.md) for laptop boot notes and hardware limitations.

The shell-first image is a hybrid BIOS/UEFI ISO for x86-64 machines. To try it on a laptop, write `build/anix-shell.iso` to a USB drive with an image writer in raw/DD mode, boot the USB entry in UEFI mode, and disable Secure Boot for that boot. This is an experimental kernel image, not a replacement for the laptop's installed system; use the firmware's one-time boot menu. A UEFI GOP framebuffer is used for the display and a PS/2-compatible keyboard controller is used for local keyboard input. Some laptops expose the built-in keyboard through that controller and some do not. There is no USB HID, touchpad, Wi-Fi, or laptop-specific graphics driver yet, so a machine without PS/2 keyboard emulation may reach the shell screen without usable local input. Keep a USB keyboard available and do not expect networking or disk access beyond the limited legacy ATA driver.

For GDB:

```sh
gdb build/anix.elf
(gdb) target remote :1234
```

## Expected serial milestone

A successful test-image boot prints `ANIX_BOOT_OK`, followed by ring-3 fixture and SDK checks, then the `anixsh>` test prompt. The fixture shell reads serial or PS/2 keyboard input. `make test-transition` emits `ANIX_TRANSITION_POST_SWITCH_OK` and `ANIX_TRANSITION_ROUNDTRIP_OK` only after candidate-root checks and rollback pass in QEMU; the normal smoke test covers permanent activation, process spawn/wait, and ring 3 startup.

The independent userspace SDK and syscall ABI are documented in [sdk/README.md](sdk/README.md). Build its example with `make sdk-demo`; the QEMU userspace test executes it during boot.

The Rust host-side Anix package tooling (`apm`, `apkg-build`, `apkg-verify`) is in [apm/](apm/README.md). It builds Anix-compatible SDK programs into `.apkg` archives and installs them only beneath an explicit sandbox root. Native execution, HTTPS repositories, and signed repository metadata are not yet supported.

Boot diagnostics send an ANSI 16-color foreground and background palette through the serial TTY, then list initialized kernel services. Successful services use green `[OK]`; optional unavailable storage uses yellow `[SKIP]`. The serial terminal needs ANSI color support to render the palette and status colors.

The optional legacy ATA PIO path and AnixFS v1 volume are exercised with `make test-storage`, including a second QEMU boot that verifies an SDK-created file survived restart. Format a whole-disk image with `python3 scripts/mkfs-anix.py path/to/disk.img`, or format an existing partition range with `python3 scripts/mkfs-anix.py path/to/disk.img START_LBA SECTOR_COUNT`. The kernel discovers MBR primary partitions and mounts the first partition containing AnixFS; whole-disk images remain supported. AnixFS is a small project-specific format, not FAT/ext compatibility.

## Toolchain note

The checked-in `third_party/limine/limine.h` is a deliberately minimal protocol surface used by the current kernel. Before changing the Limine protocol revision, compare it against the official `limine-protocol` header and update it atomically with the boot tests.

## Repository layout

See `docs/ARCHITECTURE.md` and `docs/STATUS.md`.

## License

Anix is distributed under the GNU General Public License, version 3 only
(SPDX: `GPL-3.0-only`). See [LICENSE](LICENSE). Third-party components retain
their own applicable license terms.
