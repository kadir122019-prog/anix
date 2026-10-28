# Repository format v1

Repository metadata is JSON with `metadata` and `signatures`. Metadata
contains `format_version = 1`, a monotonically increasing `version`, Unix
`expires`, and a filename-sorted list of targets. Each target binds the simple
archive filename, package name/version/release, byte size, and SHA-256. The
canonical signed bytes are compact `serde_json` serialization of the typed
`metadata` object; the envelope itself is pretty-printed JSON. The signature
is Ed25519 and carries a key ID equal to SHA-256 of the 32-byte public key.
Version 1 requires exactly one signature from the key configured by the user.

Local repositories are directories containing `.apkg` files. They may be
unsigned for administrator-managed development use or carry `repository.json`
created by `apkg-repo-sign`. HTTPS repositories fetch `repository.json` and
target archives over HTTPS with normal certificate validation. HTTPS requires
an explicitly supplied public key via `apm repo add NAME URL --key HEX`; keys
are never bootstrapped from the remote server. The client persists the highest
metadata version and signed content digest, rejects expired metadata,
decreasing versions, and same-version content changes, and verifies target
hash/size plus archive manifest before caching and again before installation.

This is a small signed-metadata design inspired by TUF concepts, not a complete
TUF implementation. It has one signing key/role, no threshold signatures,
delegations, signed root rotation, or revocation mechanism. Key replacement
requires explicit client reconfiguration. Expiration bounds stale metadata;
it does not protect a client that has never updated its trusted clock.
