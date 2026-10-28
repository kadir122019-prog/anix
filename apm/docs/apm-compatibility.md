# Anix compatibility matrix for `apm`

Inspected against the kernel, VFS, process loader, ABI headers, SDK, and current
kernel status documentation on 2026-10-28.

| Requirement | Anix status | Consequence for `apm` |
|---|---|---|
| x86-64 ELF execution | Static, non-PIE ELF64 `ET_EXEC` images can run at ring 3. Kernel ELF loader checks segment ranges and W^X. | Package payloads can contain SDK-built Anix ELF files. Linux ELF files are incompatible. |
| Native syscall ABI | ABI v1 uses `int 0x80`; SDK wrappers live in `sdk/include/anix/syscall.h`. It has process, descriptor, VFS, input, timer and poweroff calls. | `apm` cannot use Linux syscalls and no Rust `std` target is supplied for Anix. |
| Processes | `execv`, `spawnv`, `waitpid`, exit and process groups are present with bounded process records. | A Rust program using host `std` cannot be linked against these alone; no native Rust runtime/startup exists. |
| Files and directories | VFS supports open/read/write/create/list/stat, directories, cwd and unlink. Initramfs is read-only except `/tmp`; AnixFS is mounted at `/persist` when legacy ATA storage is available. | There is no writable `/usr` package root on the running kernel. Host `apm` installs into a sandbox root only. |
| Permissions/ownership | Basic node writable/persistent metadata exists; no Unix uid/gid/mode enforcement. | The archive stores conservative file modes, but kernel-enforced ownership/permissions are not available. |
| Memory | User allocator is based on `sbrk`; no general mmap syscall or shared-library loader. | A standard host Rust binary’s runtime requirements are unsupported. |
| Networking and TLS | No network device/API, sockets, TLS, or DNS. | Host `apm` can use HTTPS with reqwest/rustls and pinned signed metadata. The native Anix package manager cannot fetch remote packages. |
| Compression / crypto | Kernel has no zstd or repository-signature API. | Host `.apkg` uses zstd/SHA-256; host repository metadata uses Ed25519. These libraries are not available to native Anix programs. |
| Dynamic linking | No dynamic ELF loader or shared-library ABI. | Anix package programs must be statically linked with the Anix SDK/runtime. |

## Host toolchain and supported workflow

The Rust workspace currently targets the host and uses Rust `std`, crates.io
dependencies, filesystem APIs, and zstd. Build it on a development machine
with `make apm-test`. Build target applications
separately with the project SDK, for example:

```sh
make sdk-program ANIX_SDK_SRC=apm/examples/hello/hello.c \
  ANIX_SDK_NAME=hello ANIX_SDK_OUTPUT=build/apm-hello.elf
```

Then stage that ELF in a package source directory and run `apkg-build`. The
provided `sh apm/scripts/build-sample.sh` performs those steps. Inspect the
result using `apkg-verify` and Anix `readelf`/ELF checks before distribution.

## Native port blockers

Native `apm` requires a Rust `core`/`alloc`/`std` strategy, a supported native
Rust target and linker/startup, a complete writable package filesystem
hierarchy, robust file metadata and rename semantics, sockets, DNS, TLS,
signature libraries, and tested failure recovery. These are not present; this
workspace must not be copied into the kernel initramfs and represented as
native-ready.
