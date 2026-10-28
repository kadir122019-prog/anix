#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
debug=0; build=1
while [ "$#" -gt 0 ]; do case $1 in --debug) debug=1;; --no-build) build=0;; --) shift; break;; *) break;; esac; shift; done
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo 'error: qemu-system-x86_64 not found' >&2; exit 1; }
[ "$build" -eq 0 ] || make iso
[ -s build/anix.iso ] || { echo 'error: build/anix.iso is missing or empty' >&2; exit 1; }
set -- -M q35 -cpu max -m 256M -cdrom build/anix.iso -boot d -serial stdio -display none -no-reboot -no-shutdown "$@"
if [ "$debug" -eq 1 ]; then set -- -S -s "$@"; echo 'QEMU waiting for GDB on tcp:1234' >&2; fi
exec qemu-system-x86_64 "$@"
