#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
case ${1:-} in --clean) make clean;; '') :;; *) echo "usage: $0 [--clean]" >&2; exit 2;; esac
make check-deps
make iso
printf 'Anix ISO: %s/build/anix.iso\n' "$root"
