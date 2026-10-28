#!/usr/bin/env python3
"""Boot with a temporary IDE disk and verify kernel ATA PIO discovery."""
import socket
import subprocess
import sys
import time
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    image = root / "build/tests/ata-test.img"
    image.parent.mkdir(parents=True, exist_ok=True)
    disk_sectors = 32768
    partition_start = 2048
    partition_sectors = disk_sectors - partition_start
    with image.open("wb") as disk:
        disk.truncate(disk_sectors * 512)
        mbr = bytearray(512)
        entry = 446
        mbr[entry + 4] = 0x83
        import struct
        struct.pack_into("<II", mbr, entry + 8, partition_start, partition_sectors)
        mbr[510:512] = b"\x55\xaa"
        disk.seek(0)
        disk.write(mbr)
        disk.seek(9000 * 512)
        disk.write(bytes(index & 0xff for index in range(512)))
    subprocess.run([sys.executable, "scripts/mkfs-anix.py", str(image),
                    str(partition_start), str(partition_sectors)], cwd=root, check=True)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    command = [
        "qemu-system-x86_64", "-M", "pc", "-cpu", "max", "-m", "256M",
        "-drive", f"file={image},format=raw,if=ide,index=0", "-cdrom", "build/anix.iso",
        "-boot", "d", "-serial", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-display", "none", "-no-reboot", "-no-shutdown",
    ]
    def run_once(required_sentinel: bytes) -> str:
        process = subprocess.Popen(command, cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        serial = None
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited before opening the storage-test serial socket")
                try:
                    serial = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    serial.settimeout(0.1)
                    serial.connect(("127.0.0.1", port))
                    break
                except OSError:
                    serial.close()
                    serial = None
                    time.sleep(0.1)
            if serial is None:
                raise RuntimeError("QEMU storage-test serial socket did not become available")
            received = bytearray()
            deadline = time.monotonic() + 20
            while b"anixsh> " not in received and time.monotonic() < deadline:
                try:
                    received.extend(serial.recv(4096))
                except TimeoutError:
                    pass
            output = received.decode("utf-8", errors="replace")
            required = (b"[storage] ATA PIO disk ata0:", b"ANIX_PARTITION_BOUNDS_TEST_OK",
                        b"ANIX_BLOCK_RW_TEST_OK", b"ANIX_BOOT_OK",
                        b"ANIX_SDK_NESTED_VFS_OK", b"ANIX_SDK_CWD_OK", b"ANIX_SDK_RMDIR_OK",
                        b"ANIX_SDK_PERSIST_STAT_OK", b"[storage] MBR primary partitions: 1",
                        b"[vfs] persistent AnixFS mounted from ata0p1", required_sentinel)
            if required_sentinel == b"ANIX_SDK_PERSIST_REWRITTEN":
                required += (b"ANIX_SDK_PERSIST_DIR_OK",)
            missing = [item.decode("ascii") for item in required if item not in received]
            if missing or b"anixsh> " not in received:
                raise RuntimeError("storage QEMU boot missed " + ", ".join(missing) + "\n" + output)
            return output
        finally:
            if serial is not None:
                serial.close()
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    run_once(b"ANIX_SDK_PERSIST_CREATED")
    print("[pass] QEMU MBR partition discovery and bounded partition-backed AnixFS mount")
    print("[pass] QEMU ATA PIO sector read/write/flush/readback")
    run_once(b"ANIX_SDK_PERSIST_REWRITTEN")
    print("[pass] AnixFS reopened and rewrote a file")
    run_once(b"ANIX_SDK_PERSIST_UPDATED_RESTORED")
    print("[pass] AnixFS rewrite survived a full QEMU reboot")
    with image.open("rb") as disk:
        disk.seek(9001 * 512)
        persisted = disk.read(512)
    expected = bytes((index ^ 0xa5) & 0xff for index in range(512))
    if persisted != expected:
        raise RuntimeError("ATA sector write was not persisted to the disk image after QEMU exited")
    print("[pass] QEMU disk image retained flushed sector data")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[fail] {error}", file=sys.stderr)
        raise SystemExit(1)
