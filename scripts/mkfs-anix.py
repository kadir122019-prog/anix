#!/usr/bin/env python3
"""Create the current bounded AnixFS v1 volume on a raw disk image."""
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
SLOT_COUNT = 64
SLOT_SECTORS = 128
DATA_START = 1 + SLOT_COUNT * 2
MIN_SECTORS = DATA_START + SLOT_COUNT * SLOT_SECTORS


def checksum(record: bytearray, offset: int) -> int:
    value = 2166136261
    for index, byte in enumerate(record):
        if offset <= index < offset + 4:
            byte = 0
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def main() -> None:
    if len(sys.argv) not in (2, 4):
        raise SystemExit("usage: mkfs-anix.py IMAGE [START_LBA SECTOR_COUNT]")
    image = Path(sys.argv[1])
    start_lba = int(sys.argv[2], 0) if len(sys.argv) == 4 else 0
    volume_sectors = int(sys.argv[3], 0) if len(sys.argv) == 4 else None
    image.parent.mkdir(parents=True, exist_ok=True)
    if not image.exists():
        with image.open("wb") as disk:
            disk.truncate(16 * 1024 * 1024)
    size = image.stat().st_size
    if size % SECTOR_SIZE:
        raise SystemExit("image must be sector-aligned")
    if volume_sectors is None:
        volume_sectors = size // SECTOR_SIZE - start_lba
    if start_lba < 0 or volume_sectors < MIN_SECTORS or start_lba + volume_sectors > size // SECTOR_SIZE:
        raise SystemExit(f"volume must fit the image and contain at least {MIN_SECTORS} sectors")

    superblock = bytearray(SECTOR_SIZE)
    struct.pack_into("<8sIIIIQI", superblock, 0, b"ANIXFS01", 1, SLOT_COUNT,
                     SLOT_SECTORS, DATA_START, volume_sectors, 0)
    struct.pack_into("<I", superblock, 32, checksum(superblock, 32))
    with image.open("r+b") as disk:
        disk.seek(start_lba * SECTOR_SIZE)
        disk.write(superblock)
        disk.write(bytes((SLOT_COUNT * 2) * SECTOR_SIZE))
        disk.flush()
    print(f"formatted AnixFS v1: {image} (start {start_lba}, {volume_sectors} sectors)")


if __name__ == "__main__":
    main()
