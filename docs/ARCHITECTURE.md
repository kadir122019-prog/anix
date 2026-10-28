# Anix architecture

Anix is an initially monolithic kernel with explicit subsystem boundaries. Generic code may depend on stable subsystem interfaces under `include/anix/`; it must not include private driver internals.

## Initialization order

1. Limine transfers control directly to `kmain` in 64-bit mode.
2. COM1 is initialized and validated before other diagnostics.
3. A kernel GDT, 64-bit TSS, RSP0 stack, and double-fault IST stack are installed.
4. The IDT is loaded with exception and hardware IRQ stubs plus a DPL3 `int 0x80` gate.
5. CR0.WP is enabled, then CPUID-supported SMEP, SMAP, and UMIP are enabled before userspace entry.
6. Limine base revision and required responses are validated.
7. The boot memory map is summarized without assuming physical address zero or contiguous RAM.
8. The physical-page allocator imports aligned usable ranges and performs a real allocation.
9. A candidate kernel address space and guarded transition stack are audited; the candidate is optionally round-trip tested and then activated.
10. KVA/heap, VFS/initramfs, APIC/IOAPIC, timer, keyboard, and the scheduler are initialized.
11. `ANIX_BOOT_OK` is emitted and the scheduler starts the ring-3 initramfs test harness; it exercises kernel interfaces and then execs the unchanged `/bin/anixsh` test fixture.

## Memory model

The kernel is linked in the higher half at `0xffffffff80000000`. Early physical allocations are returned through the Limine HHDM offset. The PMM maintains a single allocation bitmap covering physical frame numbers through the highest usable address. The bitmap is stored inside and reserves pages from a usable Limine region. Non-usable gaps remain allocated. Allocation returns physical addresses explicitly; HHDM conversion is a separate operation. Invalid, unaligned, metadata, and double-free attempts are rejected. Arbitrary physical ranges can be reserved and released atomically: every frame is validated before allocator state is changed.

## Invariants

- Required boot responses are checked before dereference.
- Only `LIMINE_MEMMAP_USABLE` ranges enter the PMM.
- Region starts and ends are aligned inward to 4 KiB boundaries.
- Failed required initialization is fatal and visible over serial.
- Architecture-specific port I/O remains inside the serial driver.

## Virtual memory manager

The VMM manages independent four-level x86-64 page-table roots using physical pages supplied by the PMM. Its API distinguishes virtual addresses, physical addresses, and page flags. It supports 4 KiB, 2 MiB, and 1 GiB leaves, owned and borrowed roots, canonical-address checking, mapping, querying with byte offsets, unmapping, empty intermediate-table reclamation, CR3 activation, and targeted TLB invalidation. Single-page and multi-page map operations roll back partial state. Boot clones and validates the kernel, HHDM, framebuffer, and consumed Limine mappings before activating the kernel-owned root.

## CPU protection features

Anix requires NX and sets/verifies IA32_EFER.NXE before using NX page-table bits. CR0.WP protects read-only supervisor pages from kernel writes. CPUID-gated SMEP prevents supervisor instruction fetches from user pages; SMAP blocks supervisor data access except inside short interrupt-disabled `uaccess` copies; UMIP restricts descriptor-table and machine-status instructions at CPL3. The linker exports exact text, read-only data, writable data, and kernel-end symbols. Boot validates section mappings and builds a W^X kernel address space. User ELF pages receive permissions derived from PT_LOAD flags. SYSCALL is detected but userspace calls currently use a DPL3 `int 0x80` gate.

## Page-table ownership and import

A VMM space now explicitly records whether its root and descendant tables are owned by Anix. Created spaces are owned and may be mutated or destroyed. Imported spaces are borrowed views intended for inspection; mutating operations reject them, and destruction only detaches the view without freeing bootloader-owned tables. The walker reports leaf mappings with their virtual base, physical base, size, and flags, preserving 4 KiB, 2 MiB, and 1 GiB granularity. This is the basis for safely enumerating the current Limine address space before cloning required mappings.

## Candidate kernel address space

Anix clones selected ranges from a borrowed address space into an owned candidate. Complete aligned 2 MiB and 1 GiB leaves remain large mappings; clipped portions are split into 4 KiB mappings so permissions can be changed at section granularity. Early boot applies W^X permissions, clones the HHDM, live stacks, descriptor structures, framebuffer, and required boot data, and validates the candidate. After the round-trip path has been QEMU-tested, boot activates the candidate permanently and retains ownership for future kernel mappings.

## Live transition ranges

Architecture subsystems export the virtual ranges that must survive a CR3 transition: GDT, TSS, the Ring 0 stack, double-fault IST, IDT, current stack page, and PMM bitmap. Candidate construction clones any live range not already covered by the protected kernel image and validates every page before the candidate is accepted. The current stack export is intentionally one page and remains insufficient for activation; the final transition will use a dedicated, explicitly bounded stack.

## Candidate HHDM reconstruction

The PMM exports its normalized usable physical regions and HHDM offset without exposing allocator internals. Candidate construction walks the corresponding source HHDM ranges and reproduces their translations. Complete non-overlapping large leaves remain large; leaves overlapping an already cloned live object are split into 4 KiB mappings only where necessary. Validation checks the first and last byte of every usable region, confirms physical-address identity through the HHDM offset, and requires writable mappings. This covers all physical pages the PMM may return, including future page-table pages.

## Boot-resource preservation

The boot layer exposes required Limine-owned objects through a callback visitor instead of leaking request globals into generic VMM code. The inventory covers the memory-map response, pointer array, every entry, HHDM response, framebuffer response and pointer array, framebuffer structures, EDID blocks, and pixel buffers. All ranges are overflow-checked and page-normalized. Framebuffer MMIO is mapped writable, non-executable, and cache-disabled when Limine's framebuffer mapping cannot be cloned; the remaining resources are cloned from the boot address space and validated. These mappings remain borrowed resources; cloning their translations does not transfer ownership of their physical storage.

## CR3 transition preparation

The transition stack uses four independently allocated 4 KiB physical pages mapped at `0xfffffe0000000000`. The pages are writable and NX. The adjacent lower and upper pages remain unmapped as guards. Setup is transactional and releases every allocated page and mapping on failure. `make test-transition` explicitly enables the round-trip test: assembly records the old CR3, RSP, and RFLAGS, switches to the candidate root and stack, then calls a post-switch continuation. The continuation verifies CR3, its RSP, PMM allocation/free and HHDM access, and live GDT/IDT/TSS mappings. The assembly rollback entry restores the old CR3, RSP, callee-saved registers, and RFLAGS, then returns the test mask to the original context. QEMU must report both transition sentinels for the test target to pass. Normal boot omits the round-trip but permanently activates the candidate whose contents and mapping audit are covered by QEMU tests.

## Kernel virtual addresses and heap

The kernel virtual-address allocator manages a fixed 512 MiB higher-half window beginning at `0xffffc00000000000`. A bitmap tracks contiguous virtual runs; each page is backed by an independent PMM frame, zeroed through the HHDM, and mapped writable/NX by default. User address spaces register with KVA so new kernel mappings are synchronized into live roots and removed when freed. Failed allocations roll back both mappings and frames. Guarded allocations reserve unmapped pages on both ends; scheduler execution and privilege-transition stacks use this mode. The heap obtains four-page extents from this allocator and uses 16-byte-aligned boundary blocks with first-fit allocation, splitting, adjacent-block coalescing, address/size-bound header checks, and 8-byte trailing redzones. It poisons freed payloads and validates KVA mappings before reading candidate metadata, so invalid pointers are rejected without dereferencing unmapped addresses. These allocators are initialized only after the kernel-owned CR3 becomes active.

## Block storage

The block layer registers bounded sector devices with checked read/write ranges and a flush operation. The first driver probes the legacy primary-master ATA interface, requires LBA28 support, and performs polling PIO transfers of 512-byte sectors. It is optional at boot so machines without a legacy ATA device continue to run. A checked MBR scanner registers up to four primary partitions as block devices whose relative requests cannot escape their partition bounds. AnixFS v1 uses 64 fixed metadata slots with dual checksummed metadata records per slot and 64 KiB data extents. It mounts from a primary partition as `/persist`, with whole-disk mounting retained for existing images; QEMU tests partition discovery, persistence, rewrite, and reboot. It supports nested directories and files while preserving legacy flat-file records, but MBR extended/GPT traversal, a journal, and standard on-disk compatibility are not implemented.

## Interrupts, terminal, and scheduler

ACPI MADT parsing discovers the local APIC, IOAPICs, and ISA IRQ overrides. FADT and DSDT parsing locates the ACPI S5 sleep type and PM1 control ports for the shutdown syscall. The kernel programs xAPIC and IOAPIC routing for the local timer and PS/2 keyboard/mouse. Keyboard and pointer drivers publish normalized events to a blocking queue exposed through the SDK; the keyboard also feeds the shell's cooked character input. Terminal output is mirrored to serial and a firmware-provided framebuffer text console, with a software mouse pointer overlaid on the framebuffer. A single-core round-robin scheduler switches from APIC timer interrupts as well as explicit yields. It has an HLT idle task, blocks and wakes direct parent/child waits, exits returned tasks, and reclaims their stacks on a later switch. Every task has a separate execution stack and privilege-transition stack; both have unmapped guard pages, and the scheduler updates TSS.RSP0 and CR3 when it switches tasks. Per-process user-return records preserve the kernel stack and callee-saved registers across ring transitions. SMP locks and general-purpose wait queues remain future work.

Anonymous pipes use a fixed 4 KiB ring buffer and the scheduler wait queues: readers block until bytes arrive or all writers close, writers block while the ring is full and fail when all readers close, and every descriptor reference is inherited across spawn. `dup2` atomically replaces standard descriptors per process, so ring-3 programs can route stdin and stdout through inherited pipes without changing the terminal shell. Process groups are inherited by spawned children; `getpgid` and `setpgid` allow a process to create its own group and a parent to inspect or regroup direct children. A small `no_std` Rust policy module validates group changes against the C process-table scope. The SDK QEMU workload proves a reader blocks, receives bytes, observes EOF after close, and is reaped by its parent; it fills the ring to block a writer until the child drains space and checks write failure after the final reader closes, and checks process-group inheritance and reassignment.

The scheduler exposes intrusive FIFO wait queues with atomic enqueue-and-block and wake-one/wake-all operations. A task can wait on one queue at a time; it is removed from that queue as it becomes runnable. `wait` and `waitpid` use a per-process child-event queue and recheck child state with interrupts disabled, preventing a child exit between condition checking and blocking from being lost. Serial and keyboard IRQ handlers wake a shared terminal-input queue, so the terminal sleeps until input arrives; the UART keeps a polling fallback when IRQ4 routing is unavailable.

## VFS and userspace

PID 1 adopts children left by an exiting intermediate process, including children that have already exited, so `waitpid(-1)` can reap them. If PID 1 exits first, remaining descendants are detached and release their process records when their tasks finish. A successful `exec` updates both the scheduler's active address-space root and the saved kernel return context's CR3; this is required because `exit` returns from ring 3 through that saved context after the replaced page tables have been destroyed.

The boot module request loads a newc CPIO archive. Its nodes remain read-only, while userspace can create directories and volatile files beneath `/tmp`, and persistent files and directories in `/persist` when AnixFS is mounted. Each process has a canonical working directory; relative paths normalize `.` and `..`, and children inherit the current directory. Directory references prevent removal while any process is using a directory; `rmdir` requires an empty writable directory. Unlinked open files live until their descriptors close. AnixFS v1 keeps its sector layout and supports durable directory records alongside legacy flat file records; files can be reopened with write, append, or truncate access. The ELF64 loader accepts static `ET_EXEC` images, maps each PT_LOAD segment into a process-owned root with page permissions, clones the kernel high half, and constructs the conventional `argc`/`argv`/`envp` initial stack. The additive `execv` and `spawnv` syscalls copy bounded argument and environment vectors from the caller. Exec builds a complete replacement address space before swapping it into the process, then releases the old image pages; failed image preparation leaves the running image in place. `waitpid` can block for a selected direct child or any direct child and returns its PID, with the exit status through an optional checked user pointer. Ring 3 entry uses an IRET frame and the TSS RSP0 stack. The documented ABI v1 provides serial write, terminal line input, timer ticks, exec/execv, exit, direct-child spawn/spawnv/wait/waitpid, anonymous pipe creation, chdir/getcwd, VFS open/open_mode/read/close/list/create/mkdir/rmdir/write/seek/unlink, and cooperative yield. The SDK boot check exercises execv/spawnv argument and environment transfer, blocked and already-exited waitpid cases, failed-exec preservation, same-address image replacement, nested temporary paths, persistent directory creation and removal across reboots, file append, persistent rewrite, working-directory inheritance, and process lifecycle, blocked pipe I/O, EOF, stdin/stdout `dup2` redirection, and EOF before execing the unchanged `anixsh`. Process records are bounded to 32; dynamic linking, signals, and a standard POSIX ABI remain future work.
