#include <anix/boot.h>
#include <anix/framebuffer.h>
#include <anix/ata.h>
#include <anix/anixfs.h>
#include <anix/block.h>
#include <anix/cpu.h>
#include <anix/kernel_image.h>
#include <anix/panic.h>
#include <anix/gdt.h>
#include <anix/interrupts.h>
#include <anix/pmm.h>
#include <anix/pci.h>
#include <anix/serial.h>
#include <anix/vmm.h>
#include <anix/transition.h>
#include <anix/kva.h>
#include <anix/heap.h>
#include <anix/string.h>
#include <anix/kernel_init.h>
#include <anix/keyboard.h>
#include <anix/mouse.h>
#include <anix/scheduler.h>
#include <anix/vfs.h>
#include <anix/user.h>
#include <limine.h>
extern volatile struct limine_memmap_request anix_memmap_request;
static struct vmm_space kernel_address_space = { .root_phys = PMM_INVALID_PHYS };
static struct acpi_platform_info platform_info;
#ifdef ANIX_ENABLE_STORAGE_TEST
static void storage_block_test(void) {
    struct block_device *device = block_find("ata0");
    uint8_t input[ANIX_BLOCK_SECTOR_SIZE];
    uint8_t expected[ANIX_BLOCK_SECTOR_SIZE];
    uint8_t output[ANIX_BLOCK_SECTOR_SIZE];
    if (!device) panic("storage test requires ata0");
    for (size_t i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)i;
    if (!block_read(device, 9000u, 1u, input) || kmemcmp(input, expected, sizeof(input)) != 0)
        panic("ATA PIO sector read test failed");
    for (size_t i = 0; i < sizeof(output); ++i) output[i] = (uint8_t)(i ^ 0xa5u);
    if (!block_write(device, 9001u, 1u, output) || !block_flush(device) ||
        !block_read(device, 9001u, 1u, input) || kmemcmp(input, output, sizeof(output)) != 0)
        panic("ATA PIO sector write/readback test failed");
    struct block_device *partition = block_find("ata0p1");
    if (!partition || !block_read(partition, partition->sector_count - 1u, 1u, input) ||
        block_read(partition, partition->sector_count - 1u, 2u, input) ||
        block_write(partition, partition->sector_count - 1u, 2u, output))
        panic("partition boundary enforcement test failed");
    serial_write("ANIX_PARTITION_BOUNDS_TEST_OK\n");
    serial_write("ANIX_BLOCK_RW_TEST_OK\n");
}
#endif
#ifdef ANIX_ENABLE_TRANSITION_TEST
static struct transition_context transition_test_context;
#endif
static void init_task(void *argument) {
    (void)argument;
    if (!user_process_start("/bin/init")) panic("ring 3 init process failed");
}
void kmain(void) {
    const bool serial_detected = serial_init();
    if (serial_detected && serial_color_test()) serial_write_service_ok("ANSI TTY palette test (16 foreground and background colors)");
    serial_write("\nAnix 0.13.0 - x86_64\n"); serial_write("[boot] serial: ok\n");
    gdt_init(); serial_write("[arch] GDT/TSS: ok\n");
    idt_init(); serial_write("[arch] IDT/exceptions: ok\n");
    struct cpu_features cpu_features;
    cpu_detect_features(&cpu_features);
    serial_write("[arch] NX: "); serial_write(cpu_features.nx ? "supported" : "unsupported");
    serial_write(", SYSCALL: "); serial_write(cpu_features.syscall ? "supported" : "unsupported");
    serial_write(", 1GiB pages: "); serial_write(cpu_features.page_1g ? "supported\n" : "unsupported\n");
    if (!cpu_enable_fpu(&cpu_features)) panic("x87/SSE2/FXSAVE support is required for task context switching");
    serial_write("[arch] x87/SSE2 and FXSAVE task-state support: enabled\n");
    if (!cpu_features.nx || !cpu_enable_nx()) panic("NX support is required for kernel page protections");
    cpu_enable_hardening(&cpu_features);
    serial_write("[security] CR0.WP: enabled, SMEP: "); serial_write(cpu_features.smep ? "enabled" : "unavailable");
    serial_write(", SMAP: "); serial_write(cpu_features.smap ? "enabled" : "unavailable");
    serial_write(", UMIP: "); serial_write(cpu_features.umip ? "enabled\n" : "unavailable\n");
    struct anix_boot_info info; if(!boot_collect_info(&info)) panic("Limine boot information unavailable or incompatible");
    serial_write("[boot] memory map entries: "); serial_write_dec(info.memory_map_entries); serial_write("\n[boot] usable memory bytes: "); serial_write_dec(info.usable_bytes); serial_write("\n[boot] HHDM offset: "); serial_write_hex(info.hhdm_offset); serial_write("\n");
    if(!pmm_init(anix_memmap_request.response, info.hhdm_offset)) panic("physical memory allocator initialization failed");
    if (!kernel_image_validate_mappings(vmm_active_root())) panic("kernel image mapping validation failed");
    struct vmm_space active_space;
    struct vmm_space kernel_candidate = { .root_phys = PMM_INVALID_PHYS, .owned = false };
    if (!vmm_import_active(&active_space)) panic("active address-space import failed");
    if (!boot_cache_modules()) panic("Limine did not load the required initramfs module");
    if (!kernel_platform_discover(&active_space, &platform_info)) panic("ACPI MADT discovery failed");
    if (!kernel_image_build_candidate(&active_space, &kernel_candidate, cpu_features.nx)) panic("kernel image candidate construction failed");
    if (!kernel_candidate_clone_live_ranges(&active_space, &kernel_candidate)) panic("live-range clone failed");
    if (!kernel_candidate_clone_hhdm(&active_space, &kernel_candidate)) panic("HHDM clone failed");
    if (!kernel_candidate_clone_boot_resources(&active_space, &kernel_candidate)) panic("boot-resource clone failed");
    if (!kernel_image_validate_mappings(kernel_candidate.root_phys)) panic("kernel image candidate validation failed");
    if (!kernel_candidate_validate_live_ranges(&kernel_candidate)) panic("live-range candidate validation failed");
    if (!kernel_candidate_validate_hhdm(&kernel_candidate)) panic("HHDM candidate validation failed");
    if (!kernel_candidate_validate_boot_resources(&kernel_candidate)) panic("boot-resource candidate validation failed");
    struct transition_stack transition_stack = {0};
    if (!transition_stack_prepare(&transition_stack, &kernel_candidate)) panic("transition stack preparation failed");
    if (!transition_stack_validate(&transition_stack, &kernel_candidate)) panic("transition stack validation failed");
    serial_write("[mm] transition stack: "); serial_write_hex(transition_stack.virtual_base); serial_write("-"); serial_write_hex(transition_stack.stack_top); serial_write(" (guarded, not activated)\n");
#ifdef ANIX_ENABLE_TRANSITION_TEST
    const uint64_t transition_result = transition_run_roundtrip(&transition_test_context, &kernel_candidate, &transition_stack);
    if (transition_result != TRANSITION_TEST_ALL || transition_test_context.test_result != TRANSITION_TEST_ALL)
        panic("CR3 transition round-trip failed");
    if (vmm_active_root() != (transition_test_context.old_cr3 & VMM_PHYS_MASK)) panic("CR3 rollback did not restore original root");
    serial_write("[transition] old CR3: "); serial_write_hex(transition_test_context.old_cr3);
    serial_write(", candidate CR3: "); serial_write_hex(transition_test_context.candidate_cr3);
    serial_write(", old RSP: "); serial_write_hex(transition_test_context.old_rsp);
    serial_write(", original RFLAGS: "); serial_write_hex(transition_test_context.original_rflags);
    serial_write("; rollback verified\n");
#endif
    serial_write("[mm] transition candidate and guarded stack: ok\n");
    transition_stack_release(&transition_stack, &kernel_candidate);
    vmm_activate(&kernel_candidate);
    kernel_address_space = kernel_candidate;
    kernel_candidate = (struct vmm_space){ .root_phys = PMM_INVALID_PHYS, .owned = false };
    vmm_destroy(&active_space);
    serial_write("[mm] kernel-owned CR3 activated: "); serial_write_hex(vmm_active_root()); serial_write("\n");
    if (!kva_init(&kernel_address_space)) panic("kernel virtual-address allocator initialization failed");
    if (!kheap_init()) panic("kernel heap initialization failed");
    const bool ata_available = ata_pio_init();
    if (!ata_available) serial_write("[storage] no supported primary ATA LBA28 disk detected\n");
    struct block_device *storage = block_find("ata0");
    size_t partitions = storage ? block_scan_mbr(storage) : 0;
    if (partitions) {
        serial_write("[storage] MBR primary partitions: "); serial_write_dec(partitions); serial_write("\n");
    }
#ifdef ANIX_ENABLE_STORAGE_TEST
    storage_block_test();
#endif
    struct boot_module initramfs;
    if (!boot_find_module("initramfs", &initramfs) || !vfs_mount_initramfs(initramfs.data, initramfs.size))
        panic("initramfs mount failed");
    serial_write("[vfs] read-only initramfs mounted, bytes: "); serial_write_dec(initramfs.size); serial_write("\n");
    bool persistent_mounted = false;
    for (size_t index = 1; index <= 4u && !persistent_mounted; ++index) {
        char name[] = "ata0p0";
        name[5] = (char)('0' + index);
        struct block_device *partition = block_find(name);
        if (partition && vfs_mount_anixfs(partition)) {
            serial_write("[vfs] persistent AnixFS mounted from "); serial_write(name);
            serial_write(" at /persist, entries: "); serial_write_dec(anixfs_file_count()); serial_write("\n");
            persistent_mounted = true;
        }
    }
    if (!persistent_mounted && storage && vfs_mount_anixfs(storage)) {
        serial_write("[vfs] persistent AnixFS mounted from ata0 at /persist, entries: ");
        serial_write_dec(anixfs_file_count()); serial_write("\n");
        persistent_mounted = true;
    }
    if (storage && !persistent_mounted)
        serial_write("[vfs] no valid AnixFS volume on ata0 or its primary partitions; persistent files unavailable\n");
    if (!kernel_memory_selftest()) panic("kernel memory self-test failed");
    if (pci_init()) {
        serial_write("[pci] PCI configuration mechanism 1: ");
        serial_write_dec(pci_device_count()); serial_write(" functions discovered\n");
        const size_t report_count = pci_device_count() < 24u ? pci_device_count() : 24u;
        for (size_t index = 0; index < report_count; ++index) {
            struct pci_device_info device;
            if (!pci_get_device(index, &device)) continue;
            serial_write("[pci] "); serial_write_dec(device.bus); serial_write(":");
            serial_write_dec(device.device); serial_write("."); serial_write_dec(device.function);
            serial_write(" "); serial_write_hex(device.vendor_id); serial_write(":");
            serial_write_hex(device.device_id); serial_write(" ");
            serial_write(pci_class_name(device.class_code, device.subclass)); serial_write("\n");
        }
        if (pci_device_count() > report_count || pci_device_overflow_count() != 0u) {
            serial_write("[pci] listing capped; functions omitted: ");
            serial_write_dec(pci_device_count() - report_count + pci_device_overflow_count());
            serial_write("\n");
        }
    } else serial_write("[pci] PCI configuration mechanism 1 unavailable\n");
    if (!kernel_platform_start(&platform_info)) panic("platform interrupt initialization failed");
    const bool framebuffer_available = framebuffer_init();
    if (framebuffer_available) {
        framebuffer_boot_test();
        framebuffer_console_init();
#if defined(ANIX_ENABLE_DESKTOP)
        mouse_video_ready();
#endif
        struct framebuffer_info framebuffer;
        if (framebuffer_get_info(&framebuffer)) {
            serial_write("[boot] RGB framebuffer: "); serial_write_dec(framebuffer.width);
            serial_write("x"); serial_write_dec(framebuffer.height); serial_write(", ");
            serial_write_dec(framebuffer.bpp); serial_write(" bpp; color bars rendered\n");
        }
    } else {
        serial_write(info.framebuffer_available ?
            "[boot] framebuffer: present but unsupported; serial-only mode\n" :
            "[boot] framebuffer: unavailable (serial-only mode)\n");
    }
    if (!scheduler_init() || !scheduler_create_task("init", init_task, 0) ||
        !scheduler_create_task("idle", scheduler_idle_task, 0)) panic("kernel scheduler initialization failed");
    serial_write("[sched] preemptive kernel tasks: "); serial_write_dec(scheduler_task_count()); serial_write("\n");
    serial_write("\nServices running:\n");
    if (serial_detected) serial_write_service_ok("Serial console");
    else serial_write_service_skip("Serial console not detected");
    serial_write_service_ok("GDT and TSS");
    serial_write_service_ok("IDT and exception handlers");
    serial_write_service_ok("CPU protections and x87/SSE2 context");
    serial_write_service_ok("PMM and kernel-owned VMM");
    serial_write_service_ok("KVA and kernel heap");
    serial_write_service_ok("ACPI MADT discovery");
    if (acpi_poweroff_available()) serial_write_service_ok("ACPI S5 poweroff");
    else serial_write_service_skip("ACPI S5 poweroff unavailable");
    if (pci_available()) serial_write_service_ok("PCI device discovery");
    if (ata_available) serial_write_service_ok("ATA PIO block device");
    else serial_write_service_skip("ATA PIO block device not detected");
    serial_write_service_ok("Initramfs VFS");
    if (persistent_mounted) serial_write_service_ok("AnixFS persistent volume");
    else serial_write_service_skip("AnixFS persistent volume not mounted");
    serial_write_service_ok("Local APIC, IOAPIC, and timer");
    if (keyboard_available()) serial_write_service_ok("PS/2 keyboard input");
    else serial_write_service_skip("PS/2 keyboard not detected");
#if defined(ANIX_ENABLE_DESKTOP)
    if (mouse_available()) serial_write_service_ok("PS/2 mouse pointer");
    else serial_write_service_skip("PS/2 mouse not detected");
#else
    serial_write_service_skip("Desktop mouse input disabled");
#endif
    if (serial_available()) serial_write_service_ok(serial_rx_irq_active() ? "Serial input IRQ" : "Serial input polling fallback");
    serial_write_service_ok("Preemptive scheduler and idle task");
    serial_write_service_ok("Ring-3 test launcher");
    if (framebuffer_available) serial_write_service_ok("RGB framebuffer drawing");
    serial_write("ANIX_BOOT_OK\n"); serial_write("Anix kernel boot complete; entering ring 3 init.\n");
    scheduler_run();
}
