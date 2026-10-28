#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
[ "${1:-}" = --no-build ] || make kernel
mkdir -p build/tests
cc -std=c11 -Wall -Wextra -Werror -Iinclude tests/host/string_test.c lib/string.c -o build/tests/string_test
./build/tests/string_test
cc -std=c11 -Wall -Wextra -Werror -DANIX_HOST_TEST -Iinclude -Ithird_party/limine tests/host/pmm_test.c mm/pmm.c lib/string.c -o build/tests/pmm_test
./build/tests/pmm_test
cc -std=c11 -Wall -Wextra -Werror -DANIX_HOST_TEST -Iinclude -Ithird_party/limine tests/host/vmm_test.c mm/vmm.c mm/pmm.c lib/string.c -o build/tests/vmm_test
./build/tests/vmm_test
${RUSTC:-rustc} --edition=2021 --test kernel/rust/process_group.rs -o build/tests/process_group_test
./build/tests/process_group_test
if command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v xorriso >/dev/null 2>&1; then
    make iso
    log=build/tests/boot.log
    : > "$log"
    if command -v timeout >/dev/null 2>&1; then timeout 30 qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom build/anix.iso -boot d -serial file:"$log" -display none -no-reboot -no-shutdown >/dev/null 2>&1 || status=$?; else status=0; qemu-system-x86_64 -M q35 -m 256M -cdrom build/anix.iso -serial file:"$log" -display none & pid=$!; sleep 30; kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; fi
    grep -q ANIX_BOOT_OK "$log" || { echo 'QEMU smoke test failed; see build/tests/boot.log' >&2; exit 1; }
    grep -q 'anixsh: ring 3 userspace ready' "$log" || { echo 'Ring 3 user shell did not start; see build/tests/boot.log' >&2; exit 1; }
    grep -q '\[sched\] APIC timer preempted a running task' "$log" || { echo 'APIC timer did not preempt a running task; see build/tests/boot.log' >&2; exit 1; }
    grep -q '\[mm\] kernel heap redzone, free-poison, invalid-free, realloc, and double-free checks: ok' "$log" || { echo 'Kernel heap hardening checks failed; see build/tests/boot.log' >&2; exit 1; }
    if [ "${ANIX_TRANSITION_TEST:-0}" = 1 ]; then
        grep -q ANIX_TRANSITION_POST_SWITCH_OK "$log" || { echo 'CR3 transition continuation did not pass; see build/tests/boot.log' >&2; exit 1; }
        grep -q ANIX_TRANSITION_ROUNDTRIP_OK "$log" || { echo 'CR3 transition rollback did not pass; see build/tests/boot.log' >&2; exit 1; }
    fi
    echo '[pass] QEMU boot smoke test'
    python3 scripts/test-userspace.py
else
    echo '[skip] QEMU smoke test (qemu-system-x86_64 or xorriso unavailable)'
fi
