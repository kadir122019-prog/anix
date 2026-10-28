#!/bin/sh
set -eu
mode=${1:-all}
missing=0
check() { if command -v "$1" >/dev/null 2>&1; then printf '[ok]      %s\n' "$1"; else printf '[missing] %s\n' "$1"; missing=1; fi; }
check make
check "${RUSTC:-rustc}"
if command -v x86_64-elf-gcc >/dev/null 2>&1; then check x86_64-elf-gcc; else check gcc; printf '%s\n' '[note] host GCC fallback selected; a dedicated cross compiler is recommended.'; fi
if [ "$mode" != build ]; then
    if command -v curl >/dev/null 2>&1; then check curl; elif command -v wget >/dev/null 2>&1; then check wget; else printf '%s\n' '[missing] curl or wget'; missing=1; fi
    check xorriso
    check qemu-system-x86_64
    check python3
fi
exit "$missing"
