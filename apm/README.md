# Anix Package Manager (`apm`)

This Rust workspace provides host-side tools for building, verifying, and
managing Anix packages in an explicitly selected installation root. It does
not install into the host operating system by default. Host tooling supports
local repositories and HTTPS repositories with pinned Ed25519 metadata keys;
it does not run natively on Anix yet.

## Build and test

```sh
make apm-test
make apm-build
```

Binaries are `apm/target/release/apm`, `apkg-build`, `apkg-verify`,
`apkg-keygen`, and `apkg-repo-sign`.

## Build the example for Anix

From the repository root, with the Anix SDK toolchain available:

```sh
sh apm/scripts/build-sample.sh
```

This cross-builds the sample as a freestanding Anix ELF64 program and creates
`build/apm-repository/hello-1.0.0-1.apkg`. An ordinary Linux executable is not
an Anix executable. See [docs/apm-compatibility.md](docs/apm-compatibility.md)
and [docs/package-format.md](docs/package-format.md).

## Local isolated repository workflow

```sh
APM=apm/target/release/apm
ROOT=build/apm-test-root
$APM --root "$ROOT" repo add local build/apm-repository
$APM --root "$ROOT" update
$APM --root "$ROOT" search hello
$APM --root "$ROOT" info hello
$APM --root "$ROOT" -y install hello
$APM --root "$ROOT" list
$APM --root "$ROOT" verify hello
$APM --root "$ROOT" history
$APM --root "$ROOT" -y remove hello
```

The root is always a sandbox directory controlled by the caller. Package
destinations are restricted to `/usr/bin`, `/usr/lib`, `/usr/share`, and
`/usr/include` beneath that root. Installations reject unowned-file conflicts,
symlinked path components, and modified owned files. Transaction journals are
stored under `var/lib/apm/transactions`; `apm recover` rolls incomplete file
operations back. This is early host tooling, not yet a production trust or
crash-consistency guarantee.

## Supported commands

`update`, `search`, `info`, `install`, `remove`, `list`, `upgrade`, `verify`,
`clean`, `history`, `recover`, and `repo list/add/remove` are present. HTTPS
repositories require `--key` with a separately trusted Ed25519 public key;
local directories can be unsigned or signed. `-y` skips confirmation for the
selected sandbox root. Run `apm --help` for syntax. Detailed format and
security boundaries are in `docs/`.

## Signed repository workflow

```sh
cargo run --manifest-path apm/Cargo.toml --bin apkg-keygen -- \
  --private build/apm-signing-key --public build/apm-public-key
cargo run --manifest-path apm/Cargo.toml --bin apkg-repo-sign -- \
  --directory build/apm-repository --key build/apm-signing-key --metadata-version 1
PUBLIC_KEY=$(cat build/apm-public-key)
$APM --root "$ROOT" repo add signed-local build/apm-repository --key "$PUBLIC_KEY"
$APM --root "$ROOT" update
```

The key must be distributed to clients through a trusted channel. The
repository signer supports one key and one metadata role. Key rotation,
revocation, threshold roles, and a full TUF implementation remain future work.

The Anix package tools are distributed under GNU GPL version 3 only. See
[LICENSE](LICENSE).
