#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
make -C "$root" sdk-program ANIX_SDK_SRC=apm/examples/hello/hello.c \
    ANIX_SDK_NAME=hello ANIX_SDK_OUTPUT=build/apm-hello.elf
mkdir -p "$root/apm/examples/hello/files/usr/bin" "$root/build/apm-repository"
cp "$root/build/apm-hello.elf" "$root/apm/examples/hello/files/usr/bin/hello"
CARGO_PROFILE_DEV_DEBUG=${CARGO_PROFILE_DEV_DEBUG:-0} CARGO_INCREMENTAL=0 \
    cargo run -j 1 --manifest-path "$root/apm/Cargo.toml" --bin apkg-build -- \
    "$root/apm/examples/hello/package.toml" "$root/build/apm-repository/hello-1.0.0-1.apkg"
