#!/usr/bin/env python3
"""Exercise the ring-3 shell and read-only VFS through QEMU's serial socket."""
import socket
import subprocess
import sys
import time
from pathlib import Path


def main() -> None:
    stdin_test = "--stdin-test" in sys.argv[1:]
    root = Path(__file__).resolve().parent.parent
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    command = [
        "qemu-system-x86_64", "-M", "q35", "-cpu", "max", "-m", "256M",
        "-cdrom", "build/anix.iso", "-boot", "d", "-serial",
        f"tcp:127.0.0.1:{port},server=on,wait=off", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]
    process = subprocess.Popen(command, cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    serial = None
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before opening the serial socket")
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
            raise RuntimeError("QEMU serial socket did not become available")
        received = bytearray()
        deadline = time.monotonic() + 10
        stdin_sent = False
        while b"anixsh> " not in received and time.monotonic() < deadline:
            try:
                received.extend(serial.recv(4096))
            except TimeoutError:
                pass
            if stdin_test and not stdin_sent and b"ANIX_SDK_STDIN_READY" in received:
                serial.sendall(b"fd0 line test\nsdk getline\nfd0 dup\n")
                stdin_sent = True
        if b"anixsh> " not in received:
            raise RuntimeError("ring-3 shell prompt did not appear")
        serial.sendall(
            b"help\rticks\rstatus\rpwd\r"
            b"echo \"hello from anixsh\"\recho escaped\\ space\rc\t\t /tmp\rpwd\r"
            b"ls /etc\rcat /etc/motd\rcat /missing\rcd /\rpwd\r"
            b"c\t\t\t\runame\rnot-a-command\recho \"unterminated\rexit\r"
        )
        deadline = time.monotonic() + 10
        while (b"[sched] reaped task init" not in received or
               b"ANIX_SDK_REPARENT_OK" not in received) and time.monotonic() < deadline:
            try:
                chunk = serial.recv(4096)
                if not chunk:
                    break
                received.extend(chunk)
            except TimeoutError:
                pass
        output = received.decode("utf-8", errors="replace")
        required = (
            "anix-sdk: hello from a separately built ring-3 program",
            "ANIX_SDK_PROCESS_ID_OK", "ANIX_SDK_CHILD_IDS_OK", "ANIX_SDK_ORPHAN_PPID_OK",
            "ANIX_SDK_ARGV_OK",
            "ANIX_SDK_STDIO_OK",
            "ANIX_SDK_STAT_OK",
            "ANIX_SDK_DUP_OK",
            "ANIX_SDK_DUP_STDOUT_OK",
            "ANIX_SDK_SBRK_OK",
            "ANIX_SDK_ALLOCATOR_OK",
            "ANIX_SDK_SPAWNV_OK",
            "ANIX_SDK_WAITPID_OK", "ANIX_SDK_PIPE_BLOCKING_EOF_OK",
            "ANIX_SDK_PIPE_BACKPRESSURE_OK", "ANIX_SDK_PIPE_BROKEN_WRITE_OK",
            "ANIX_SDK_DUP2_STDIN_PIPE_OK", "ANIX_SDK_DUP2_STDOUT_PIPE_OK",
            "ANIX_SDK_PROCESS_GROUP_OK",
            "ANIX_SDK_SLEEP_OK", "ANIX_SDK_SHORT_SLEEP_OK",
            "ANIX_SDK_SIMD_CONTEXT_OK", "ANIX_SDK_SIMD_SCHEDULING_OK",
            "ANIX_SDK_EXEC_FAILURE_OK",
            "ANIX_SDK_MALFORMED_ELF_REJECTED",
            "ANIX_SDK_EXEC_REPLACEMENT_OK",
            "ANIX_SDK_FILE_IO_OK", "ANIX_SDK_YIELD_OK", "ANIX_SDK_RAMFS_OK", "ANIX_SDK_NESTED_VFS_OK",
            "ANIX_SDK_CWD_OK", "ANIX_SDK_FD_INHERIT_OK",
            "ANIX_SDK_RMDIR_OK",
            "ANIX_SDK_PERSIST_UNAVAILABLE",
            "ANIX_SDK_SPAWN_FAILURE_OK", "ANIX_SDK_WORKER_RAN", "ANIX_SDK_PROCESS_WAIT_OK",
            "ANIX_SDK_ORPHAN_PARENT_STARTED", "ANIX_SDK_ORPHAN_PPID_OK",
            "ANIX_SDK_ORPHAN_FINISHED", "ANIX_SDK_REPARENT_OK",
            "serial RX IRQ: active",
            "ticks: ", "etc/", "Anix: a small x86-64 kernel under active development.",
            "commands: help, ticks, status, pwd, cd [directory], ls [directory], cat file..., echo [text...], clear, uname, shutdown [--now], exit; run programs by name from /bin or by path (Tab cycles built-ins)",
            "Type help for commands; Tab cycles command names.",
            "cwd: /", "cwd: /tmp", "pid: 1  pgrp: 1", "hello from anixsh", "escaped space", "motd",
            "cat: cannot open /missing", "Anix kernel x86_64", "command not found: not-a-command",
            "syntax error: unmatched quote, escape, or too many arguments", "\x1b[0m\x1b[2J\x1b[H", "anixsh: exiting",
            "anixsh> ", "\x1b[1;32mcommands: help, ticks, status, pwd, cd [directory], ls [directory], cat file..., echo [text...], clear, uname, shutdown [--now], exit; run programs by name from /bin or by path",
            "ANIX_TTY_COLOR_TEST_OK", "Services running:", "\x1b[32m[OK]\x1b[0m Serial console",
            "[user] process returned to kernel scheduler", "[sched] reaped task init",
        )
        green_ok = "\x1b[32m[OK]\x1b[0m "
        required += tuple(green_ok + service for service in (
            "Serial console", "GDT and TSS", "IDT and exception handlers",
            "CPU protections and x87/SSE2 context", "PMM and kernel-owned VMM",
            "KVA and kernel heap", "ACPI MADT discovery", "Initramfs VFS",
            "ACPI S5 poweroff",
            "Local APIC, IOAPIC, and timer", "PS/2 keyboard input",
            "Serial input IRQ", "Preemptive scheduler and idle task", "Ring-3 test launcher",
        ))
        required += (
            green_ok + "ANSI TTY palette test (16 foreground and background colors)",
            "\x1b[33m[SKIP]\x1b[0m ATA PIO block device not detected",
            "\x1b[33m[SKIP]\x1b[0m AnixFS persistent volume not mounted",
        )
        if stdin_test:
            required += ("ANIX_SDK_STDIN_READY", "ANIX_SDK_STDIN_OK")
        missing = [item for item in required if item not in output]
        ansi_palette = [f"\x1b[{code}m" for code in (*range(30, 38), *range(90, 98), *range(40, 48), *range(100, 108))]
        missing.extend(f"ANSI color {code}" for code, sequence in zip(
            (*range(30, 38), *range(90, 98), *range(40, 48), *range(100, 108)), ansi_palette
        ) if sequence not in output)
        if "\x1b[1;36manixsh> " in output:
            missing.append("plain (non-cyan) anixsh prompt")
        if output.count("ANIX_SDK_WORKER_RAN") < 8:
            missing.append("eight repeated SDK worker exits")
        if missing:
            raise RuntimeError("userspace QEMU check missed: " + ", ".join(missing) + "\n" + output)
        print("[pass] QEMU SDK app, ring-3 syscalls, heap allocator, nested VFS directories/files, and process exit" +
              (", including read(0)" if stdin_test else ""))
    finally:
        if serial is not None:
            serial.close()
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # report a concise test failure to make
        print(f"[fail] {error}", file=sys.stderr)
        raise SystemExit(1)
