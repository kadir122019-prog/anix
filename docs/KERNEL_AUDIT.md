# Kernel completeness audit

Audit date: 2026-10-28. The findings below come from the current source and the
commands listed in the verification section; a feature is not called verified
solely because a module or status note exists.

## Subsystem findings

| Area | Classification | Evidence and limits |
|---|---|---|
| A. Boot and initialization | **Implemented and verified** for the configured QEMU path; broader firmware compatibility is **implemented but insufficiently tested** | Limine requests and boot-resource checks are in `boot/limine.c`; initialization order is in `kernel/main.c`; linker permissions are in `arch/x86_64/linker.ld`; `scripts/test.sh` requires the boot sentinel and user shell readiness. QEMU BIOS/UEFI image paths exist. VMware has been exercised in a documented profile; physical systems have not. |
| B. CPU and exceptions | **Partially implemented** | GDT/TSS, IDT exception stubs, panic diagnostics, CPUID-gated protections, APIC/IOAPIC and ACPI discovery exist. QEMU boots and preempts. AP startup, per-CPU state and SMP interrupt routing are missing; x2APIC/firmware combinations beyond tested QEMU remain insufficiently tested. |
| C. Memory management | **Implemented and verified** for tested paths; advanced stress/security coverage is **insufficient** | PMM/VMM host tests cover allocation, reservation, overflow, mapping, unmapping and table reclamation. Boot exercises kernel address-space cloning, KVA, heap, guarded stacks, user address spaces and `sbrk`. The opt-in CR3 test validates continuation, stack, PMM/HHDM, descriptor tables and rollback. No general-purpose sanitizer or exhaustive fault-injection suite was run. |
| D. Interrupts, timers and scheduling | **Partially implemented** | APIC timer preemption, cooperative yield, sleep, wait queues, idle task and task cleanup are integrated and exercised by QEMU. Scheduling and locks are single-core; no SMP-safe scheduler, per-CPU data or interprocessor wakeups exist. |
| E. Processes and user mode | **Partially implemented** | Static ELF64 `ET_EXEC`, ring 3, per-process page tables, stack/argv/envp, spawn/exec/exit/wait, process groups and user pointer checks are exercised by SDK QEMU workload. Process/task counts are bounded; one task per process; no dynamic ELF, demand paging, signals or kernel threads. Malformed and missing-image rejection now also has an explicit QEMU assertion. |
| F. Native ABI and syscalls | **Partially implemented** | ABI v1 and syscall numbers are documented in `include/anix/syscall_abi.h` and `sdk/include/anix`; syscalls cover basic descriptors, files, process lifecycle, pipes, input, time, and poweroff. Integration tests cover these paths. This is a native project ABI, not POSIX; memory mapping, signals, threads, and stable compatibility policy beyond the current version are absent. |
| G. Filesystem and storage | **Partially implemented** | VFS, read-only newc initramfs, writable `/tmp`, bounded file descriptors, and AnixFS v1 are implemented. The opt-in QEMU storage test verifies MBR partition bounds, ATA PIO read/write/flush, filesystem rewrite and persistence over reboot. Storage is limited to one legacy primary-master ATA device and a small project-specific filesystem; no GPT, journal, AHCI or NVMe. |
| H. Drivers | **Partially implemented** | Serial, Limine framebuffer, PS/2 keyboard, opt-in PS/2 mouse, PCI mechanism-1 enumeration, and legacy ATA exist. PCI devices are enumerated but not bound through a generic driver model. No USB HID/xHCI, virtio, GPU acceleration, Wi-Fi, or broad laptop driver support. |
| I. Networking | **Missing** | PCI can report network-class devices, but there is no network-device API, Ethernet/IP stack, socket ABI or network test. It depends on a driver binding model and packet-buffer ownership/concurrency design. |
| J. Security and reliability | **Partially implemented** | W^X user ELF mappings, NX, CR0.WP, supported SMEP/SMAP/UMIP, guarded stacks, bounded syscalls, safe user-copy helpers and transactional exec are present. QEMU now checks rejection of a malformed non-ELF image. There is no comprehensive adversarial fuzzing, failure-injection campaign, audit of every driver path, or security proof. |
| K. User-space readiness | **Partially implemented** | PID 1, native ELF loader, syscall ABI, SDK/runtime, initramfs, ring-3 shell, file/process APIs and QEMU integration test exist. Anix can boot to a usable development shell in tested QEMU/VMware configurations. It is not a general-purpose laptop-ready Unix system: device, networking, compatibility, packaging and utility coverage remain limited; `apm` is not implemented. |

## Change made during this audit

The loader already rejected files shorter than an ELF64 header, but the QEMU
SDK workload only checked that an executable path was absent. Added
`userspace/initramfs/etc/not-an-elf`; the SDK workload now requires both
`spawn` and `execv` to reject it and continue running. The QEMU test requires
the `ANIX_SDK_MALFORMED_ELF_REJECTED` sentinel. This tests malformed-image
handling through the real VFS, loader, process and syscall paths; it does not
replace parser fuzzing.

## Verification performed

All commands were run from the project directory inside WSL (`archlinux`):

```sh
make test
make test-transition
make test-storage
```

Results: `make test` passed the C string, PMM and VMM host tests, all three Rust
process-group policy tests, QEMU boot smoke, and the ring-3 SDK/userspace suite
including the new malformed-ELF rejection check. `make test-transition` passed
the boot and userspace suites with the transition test enabled; the script
requires both post-switch and rollback sentinels. `make test-storage` passed
partition-bounds, ATA sector read/write/flush/readback, AnixFS rewrite, and
reboot-persistence checks. These results are QEMU evidence, not proof of
physical-hardware compatibility.

`make iso-shell` built `build/anix-shell.iso`; a separate 12-second QEMU boot
check found both `ANIX_BOOT_OK` and the ring-3 `anixsh> ` prompt in its serial
log. QEMU was stopped after those sentinels appeared.

Build and boot commands:

```sh
make iso-shell                 # build build/anix-shell.iso
qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom build/anix-shell.iso -boot d -serial stdio -display none -no-reboot -no-shutdown
make iso                       # build the default SDK-test ISO as build/anix.iso
make run                       # boot the default ISO in QEMU
make test                      # host and QEMU regression suite
make test-transition           # opt-in CR3 round-trip test
make test-storage              # opt-in QEMU ATA/AnixFS persistence test
```

## Remaining work in dependency order

1. Add structured boot/CPU-local state and AP startup, then make allocators,
   scheduler, interrupt routing, and process-table synchronization SMP-safe.
2. Add PCI configuration-space access through ACPI MCFG/ECAM and a real device
   registration/binding lifecycle; implement virtio-blk before broader modern
   storage drivers.
3. Expand storage format validation and crash consistency, then add GPT and a
   journal before treating persistent data as robust.
4. Add signals, multiple threads per process, memory mapping, and a stable
   syscall error/version policy before expanding the SDK ABI.
5. Add an Ethernet driver and network stack only after driver, buffer ownership,
   concurrency, and socket interfaces are in place.
6. Add USB HID and real-hardware validation before describing Anix as suitable
   for ordinary laptops. Current QEMU/VMware boot success does not establish
   that claim.

The next recommended milestone is per-CPU/AP startup plus an SMP-safe locking
and interrupt model. This is a foundational architectural change; networking,
multicore claims, and broad hardware support should wait for it.
