#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dest="$root/third_party/limine/bin"
version=${LIMINE_VERSION:-v12.9.4}
[ -x "$dest/limine" ] && [ -f "$dest/limine-bios.sys" ] && exit 0
tmp="$root/build/limine-download"
rm -rf "$tmp"
mkdir -p "$root/build" "$dest" "$tmp"
archive="$tmp/limine-binary.tar.gz"
url="https://github.com/limine-bootloader/limine/releases/download/$version/limine-binary.tar.gz"
if command -v curl >/dev/null 2>&1; then
    curl --fail --location --retry 2 --silent --show-error "$url" --output "$archive"
elif command -v wget >/dev/null 2>&1; then
    wget -q "$url" -O "$archive"
else
    echo 'error: curl or wget is required to obtain Limine' >&2
    exit 1
fi
tar -xzf "$archive" -C "$tmp"
src="$tmp/limine-binary"
make -C "$src"
for f in limine limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI; do
    [ -f "$src/$f" ] || { echo "error: Limine asset missing: $f" >&2; exit 1; }
    cp "$src/$f" "$dest/$f"
done
chmod +x "$dest/limine"
rm -rf "$tmp"
