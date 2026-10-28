use apm::repository::{self, Metadata, Target};
use ed25519_dalek::SigningKey;

fn sample_metadata(version: u64, expires: u64) -> Metadata {
    Metadata {
        format_version: 1,
        version,
        expires,
        packages: vec![Target {
            file: "hello-1.0.0-1.apkg".into(),
            name: "hello".into(),
            version: "1.0.0".into(),
            release: 1,
            size: 512,
            sha256: "a".repeat(64),
        }],
    }
}

#[test]
fn repository_signatures_require_the_pinned_key_and_reject_tampering() {
    let key = SigningKey::from_bytes(&[17; 32]);
    let trusted = hex::encode(key.verifying_key().as_bytes());
    let bytes = repository::sign(sample_metadata(3, repository::now() + 3600), &key).unwrap();
    let (verified, digest) = repository::verify(&bytes, &trusted, 2).unwrap();
    assert_eq!(verified.version, 3);
    assert_eq!(digest.len(), 64);
    assert!(repository::verify(&bytes, &hex::encode([99u8; 32]), 0).is_err());

    let mut tampered: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    tampered["metadata"]["version"] = serde_json::json!(4);
    assert!(repository::verify(&serde_json::to_vec(&tampered).unwrap(), &trusted, 0).is_err());
}

#[test]
fn repository_expiry_rollback_and_unsafe_targets_are_rejected() {
    let key = SigningKey::from_bytes(&[23; 32]);
    let trusted = hex::encode(key.verifying_key().as_bytes());
    let expired = repository::sign(sample_metadata(2, repository::now() - 1), &key).unwrap();
    assert!(repository::verify(&expired, &trusted, 0)
        .unwrap_err()
        .to_string()
        .contains("expired"));

    let old = repository::sign(sample_metadata(2, repository::now() + 3600), &key).unwrap();
    assert!(repository::verify(&old, &trusted, 3)
        .unwrap_err()
        .to_string()
        .contains("rollback"));

    let mut unsafe_metadata = sample_metadata(4, repository::now() + 3600);
    unsafe_metadata.packages[0].file = "../escape.apkg".into();
    let unsafe_signed = repository::sign(unsafe_metadata, &key).unwrap();
    assert!(repository::verify(&unsafe_signed, &trusted, 0).is_err());
}
