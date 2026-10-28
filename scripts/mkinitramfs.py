#!/usr/bin/env python3
"""Build a deterministic, read-only newc initramfs module."""
import os
import stat
import sys
from pathlib import Path


def align4(value: int) -> int:
    return (value + 3) & ~3


def append_entry(output: bytearray, ino: int, name: str, mode: int, data: bytes) -> None:
    encoded_name = name.encode("utf-8") + b"\0"
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded_name), 0]
    output.extend(b"070701" + b"".join(f"{field:08x}".encode("ascii") for field in fields))
    output.extend(encoded_name)
    output.extend(b"\0" * (align4(len(output)) - len(output)))
    output.extend(data)
    output.extend(b"\0" * (align4(len(output)) - len(output)))


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: mkinitramfs.py ROOT OUTPUT")
    root = Path(sys.argv[1]).resolve()
    output_path = Path(sys.argv[2])
    entries = [root, *sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix())]
    archive = bytearray()
    for ino, path in enumerate(entries, 1):
        relative = "." if path == root else path.relative_to(root).as_posix()
        if path.is_dir():
            mode = stat.S_IFDIR | 0o755
            data = b""
        elif path.is_file():
            mode = stat.S_IFREG | (0o755 if os.access(path, os.X_OK) else 0o644)
            data = path.read_bytes()
        else:
            continue
        append_entry(archive, ino, relative, mode, data)
    append_entry(archive, len(entries) + 1, "TRAILER!!!", 0, b"")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(archive)


if __name__ == "__main__":
    main()
