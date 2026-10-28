use crate::{
    package::{sha256, verify_package, MAX_PACKAGE_BYTES},
    ApmError, Result,
};
use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Target {
    pub file: String,
    pub name: String,
    pub version: String,
    pub release: u64,
    pub size: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Metadata {
    pub format_version: u32,
    pub version: u64,
    pub expires: u64,
    pub packages: Vec<Target>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SignedMetadata {
    pub metadata: Metadata,
    pub signatures: Vec<SignatureRecord>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SignatureRecord {
    pub keyid: String,
    pub signature: String,
}

pub fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

pub fn parse_public_key(value: &str) -> Result<VerifyingKey> {
    let bytes = hex::decode(value.trim())
        .map_err(|e| ApmError::Transaction(format!("invalid trusted Ed25519 public key: {e}")))?;
    let array: [u8; 32] = bytes.try_into().map_err(|_| {
        ApmError::Transaction("Ed25519 public keys must be 32 bytes (64 hex characters)".into())
    })?;
    VerifyingKey::from_bytes(&array)
        .map_err(|e| ApmError::Transaction(format!("invalid Ed25519 public key: {e}")))
}

pub fn key_id(key: &VerifyingKey) -> String {
    sha256(key.as_bytes())
}

pub fn verify(bytes: &[u8], trusted_key: &str, minimum_version: u64) -> Result<(Metadata, String)> {
    if bytes.len() > 4 * 1024 * 1024 {
        return Err(ApmError::InvalidPackage(
            "repository metadata exceeds 4 MiB".into(),
        ));
    }
    let document: SignedMetadata = serde_json::from_slice(bytes)
        .map_err(|e| ApmError::InvalidPackage(format!("malformed repository metadata: {e}")))?;
    if document.signatures.len() != 1 {
        return Err(ApmError::InvalidPackage(
            "repository v1 requires exactly one trusted signature".into(),
        ));
    }
    let key = parse_public_key(trusted_key)?;
    let expected_key_id = key_id(&key);
    let signature_record = document
        .signatures
        .iter()
        .find(|signature| signature.keyid == expected_key_id)
        .ok_or_else(|| {
            ApmError::InvalidPackage(
                "repository metadata has no signature from the configured trusted key".into(),
            )
        })?;
    if document.metadata.format_version != 1 {
        return Err(ApmError::InvalidPackage(
            "unsupported repository metadata format".into(),
        ));
    }
    if document.metadata.version == 0 || document.metadata.version < minimum_version {
        return Err(ApmError::InvalidPackage(format!("repository metadata rollback or invalid version: {} is below trusted version {minimum_version}", document.metadata.version)));
    }
    if document.metadata.expires <= now() {
        return Err(ApmError::InvalidPackage(
            "repository metadata has expired".into(),
        ));
    }
    if document.metadata.packages.len() > 4096 {
        return Err(ApmError::InvalidPackage(
            "repository contains too many package targets".into(),
        ));
    }
    let mut total_size = 0u64;
    let mut previous: Option<&str> = None;
    for target in &document.metadata.packages {
        if target.file.is_empty()
            || target.file.len() > 128
            || !target.file.ends_with(".apkg")
            || !target
                .file
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'-'))
        {
            return Err(ApmError::InvalidPackage(format!(
                "unsafe repository target filename {:?}",
                target.file
            )));
        }
        if target.size == 0
            || target.size > MAX_PACKAGE_BYTES
            || target.sha256.len() != 64
            || !target.sha256.bytes().all(|c| c.is_ascii_hexdigit())
        {
            return Err(ApmError::InvalidPackage(format!(
                "invalid size/hash for repository target {}",
                target.file
            )));
        }
        total_size = total_size
            .checked_add(target.size)
            .ok_or_else(|| ApmError::InvalidPackage("repository size total overflow".into()))?;
        if total_size > 2 * 1024 * 1024 * 1024 {
            return Err(ApmError::InvalidPackage(
                "repository package total exceeds 2 GiB".into(),
            ));
        }
        if previous.is_some_and(|p| p >= target.file.as_str()) {
            return Err(ApmError::InvalidPackage(
                "repository targets must be sorted and unique".into(),
            ));
        }
        previous = Some(&target.file);
    }
    let signed = serde_json::to_vec(&document.metadata)
        .map_err(|e| ApmError::Serialization(e.to_string()))?;
    let signature_bytes = hex::decode(&signature_record.signature).map_err(|e| {
        ApmError::InvalidPackage(format!("invalid metadata signature encoding: {e}"))
    })?;
    let signature = Signature::from_slice(&signature_bytes)
        .map_err(|e| ApmError::InvalidPackage(format!("invalid metadata signature: {e}")))?;
    key.verify(&signed, &signature).map_err(|_| {
        ApmError::InvalidPackage("repository metadata signature verification failed".into())
    })?;
    Ok((document.metadata, sha256(&signed)))
}

pub fn sign(metadata: Metadata, signing_key: &SigningKey) -> Result<Vec<u8>> {
    if metadata.format_version != 1 || metadata.version == 0 {
        return Err(ApmError::InvalidPackage(
            "cannot sign unsupported or zero-version metadata".into(),
        ));
    }
    let bytes =
        serde_json::to_vec(&metadata).map_err(|e| ApmError::Serialization(e.to_string()))?;
    let signature = signing_key.sign(&bytes);
    let document = SignedMetadata {
        metadata,
        signatures: vec![SignatureRecord {
            keyid: key_id(&signing_key.verifying_key()),
            signature: hex::encode(signature.to_bytes()),
        }],
    };
    serde_json::to_vec_pretty(&document).map_err(|e| ApmError::Serialization(e.to_string()))
}

pub fn sign_directory(
    directory: &Path,
    version: u64,
    expires: u64,
    signing_key: &SigningKey,
) -> Result<Vec<u8>> {
    if expires <= now() {
        return Err(ApmError::InvalidPackage(
            "metadata expiry must be in the future".into(),
        ));
    }
    let mut paths = Vec::new();
    for entry in fs::read_dir(directory)? {
        let entry = entry?;
        if entry.file_type()?.is_file() && entry.path().extension().is_some_and(|ext| ext == "apkg")
        {
            paths.push(entry.path());
        }
    }
    paths.sort();
    let mut packages = Vec::new();
    for path in paths {
        let verified = verify_package(&path)?;
        let bytes = fs::read(&path)?;
        let file = path
            .file_name()
            .ok_or_else(|| ApmError::InvalidPackage("package has no filename".into()))?
            .to_string_lossy()
            .to_string();
        packages.push(Target {
            file,
            name: verified.manifest.package.name,
            version: verified.manifest.package.version,
            release: verified.manifest.package.release,
            size: bytes.len() as u64,
            sha256: sha256(&bytes),
        });
    }
    packages.sort_by(|a, b| a.file.cmp(&b.file));
    sign(
        Metadata {
            format_version: 1,
            version,
            expires,
            packages,
        },
        signing_key,
    )
}

pub fn signing_key_from_hex_file(path: &Path) -> Result<SigningKey> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let metadata = fs::metadata(path)?;
        if metadata.permissions().mode() & 0o077 != 0 {
            return Err(ApmError::Transaction(
                "refusing to load a private signing key readable by group/other users".into(),
            ));
        }
    }
    let text = fs::read_to_string(path)?;
    let bytes = hex::decode(text.trim())
        .map_err(|e| ApmError::Transaction(format!("invalid signing key file: {e}")))?;
    let seed: [u8; 32] = bytes.try_into().map_err(|_| {
        ApmError::Transaction("private Ed25519 seed must be 32 bytes (64 hex characters)".into())
    })?;
    Ok(SigningKey::from_bytes(&seed))
}
