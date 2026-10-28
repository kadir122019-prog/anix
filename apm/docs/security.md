# Security model and current limits

## Current trust boundary

The host `apm` tool manages only the explicitly selected `--root` directory;
its default is the relative `apm-root`, never `/`. Archive paths are checked,
only regular tar members are accepted, payload sizes are bounded, manifest
paths are restricted to approved `/usr` subtrees, file hashes are checked,
and installation refuses symlinked path components, unowned-file replacement,
and removal/replacement of locally modified owned files. Packages do not run
install hooks.

Each install/remove records a journal before changing files, moves replaced
files to a transaction backup area, commits database updates with a temporary
file and rename, and can restore incomplete transactions using `apm recover`.
The filesystem and database do not share an atomic transaction. Power loss,
hostile concurrent filesystem mutation, storage-controller failure, or a
corrupted journal can still require manual recovery. This implementation is
not yet production-grade crash consistency.

## Authentication and repository risks

SHA-256 inside a package is an integrity check only. HTTPS repositories are
authenticated by a public key that the user must pin separately; HTTPS uses
normal certificate validation and rejects downgrade redirects. Signed
metadata expires, has a monotonically increasing version, and binds each
package's size, hash, and identity. The client stores the last accepted
metadata version and digest. `apkg-keygen` creates a private signing key with
owner-only permissions on Unix; protect and back it up offline. Never put the
private key in a public repository.

Local unsigned repositories are treated as administrator-controlled trusted
input. Do not install packages from an untrusted directory. The metadata
scheme is not full TUF: there are no threshold roles, delegations, key
rotation, revocation, or protection for a client that has never synchronized
its clock. A key change must be delivered through a separate trusted channel.

## Installer limitations

Configuration-file preservation, shared-file ownership, package script hooks,
filesystem permission enforcement by Anix, and rollback across a power loss at
every possible instruction boundary are not implemented. The manager rejects
file ownership conflicts instead of sharing files. It refuses to overwrite
untracked files and refuses to remove modified files. Keep backups of valuable
data; do not use host root paths as the test installation root.
