# `.apkg` package format v1

An `.apkg` is a zstd-compressed tar archive. The tar stream begins with one
UTF-8 `manifest.toml`, followed by regular files under `payload/`. Directory,
symlink, hard-link, device, duplicate, absolute, and traversal members are
rejected. The archive has a 128 MiB compressed and uncompressed limit and at
most 4096 payload files.

The manifest declares `format_version = 1`, a `[package]` record (`name`,
SemVer `version`, positive integer `release`, `arch`, `abi`, `description`,
`license`), dependency and optional-dependency maps, conflict/provide names,
and `[[files]]` entries. Each built file entry carries its archive path,
absolute install destination, lowercase SHA-256 digest, and mode (`0644` or
`0755`). Destinations must be beneath `/usr/bin`, `/usr/lib`, `/usr/share`, or
`/usr/include`.

Files installed under `/usr/bin` must be static x86-64 ELF64 little-endian
`ET_EXEC` images with bounded load segments, no dynamic loader, no W+X segment,
no executable stack, and an entry point inside an executable segment. This
matches the current Anix loader/SDK; resources may be installed in the other
approved directories.

Package definitions use the same TOML schema but file entries include a safe,
package-root-relative `source`. The builder strips `source`, assigns stable
payload member names, hashes content, emits deterministic tar metadata, and
verifies the result before returning success. Package versions follow SemVer
2.0.0 precedence; `release` breaks ties for repackaging the same version.
Dependencies are SemVer requirements such as `">=1.2, <2"`.

Example definition:

```toml
format_version = 1

[package]
name = "hello"
version = "1.0.0"
release = 1
arch = "x86_64"
abi = "anix-1"
description = "A small Anix SDK program"
license = "GPL-3.0-only"

[[files]]
archive_path = ""
destination = "/usr/bin/hello"
sha256 = ""
mode = 493
source = "files/usr/bin/hello"
```

The package archive is internally integrity checked. It is **not publisher
authenticated**: an attacker able to replace the archive can replace both the
payload and its manifest hash. Do not treat an unsigned `.apkg` or hash as a
trusted distribution artifact.
