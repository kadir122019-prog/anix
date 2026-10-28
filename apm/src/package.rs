use crate::{ApmError, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::{Read, Write},
    path::{Component, Path, PathBuf},
};

pub const FORMAT_VERSION: u32 = 1;
pub const MAX_PACKAGE_BYTES: u64 = 128 * 1024 * 1024;
pub const MAX_PACKAGE_FILES: usize = 4096;
const ARCHIVE_MANIFEST: &str = "manifest.toml";

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct PackageMetadata {
    pub name: String,
    pub version: String,
    pub release: u64,
    pub arch: String,
    pub abi: String,
    pub description: String,
    pub license: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct PackageFile {
    pub archive_path: String,
    pub destination: String,
    pub sha256: String,
    pub mode: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Manifest {
    pub format_version: u32,
    pub package: PackageMetadata,
    #[serde(default)]
    pub dependencies: BTreeMap<String, String>,
    #[serde(default)]
    pub optional_dependencies: BTreeMap<String, String>,
    #[serde(default)]
    pub conflicts: Vec<String>,
    #[serde(default)]
    pub provides: Vec<String>,
    #[serde(default)]
    pub files: Vec<PackageFile>,
}

#[derive(Debug)]
pub struct VerifiedPackage {
    pub manifest: Manifest,
    pub payload: BTreeMap<String, Vec<u8>>,
}

pub fn sha256(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

pub fn validate_anix_elf(bytes: &[u8]) -> Result<()> {
    let malformed =
        |reason: &str| ApmError::InvalidPackage(format!("invalid Anix executable ELF64: {reason}"));
    if bytes.len() < 64
        || &bytes[..4] != b"\x7fELF"
        || bytes[4] != 2
        || bytes[5] != 1
        || bytes[6] != 1
    {
        return Err(malformed(
            "expected a complete 64-bit little-endian ELF header",
        ));
    }
    let u16_at = |offset: usize| -> u16 { u16::from_le_bytes([bytes[offset], bytes[offset + 1]]) };
    let u32_at = |offset: usize| -> u32 {
        u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
    };
    let u64_at = |offset: usize| -> u64 {
        u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
    };
    if u16_at(16) != 2
        || u16_at(18) != 62
        || u32_at(20) != 1
        || u16_at(52) != 64
        || u16_at(54) != 56
    {
        return Err(malformed("expected static x86_64 ET_EXEC ABI image"));
    }
    let entry = u64_at(24);
    let phoff =
        usize::try_from(u64_at(32)).map_err(|_| malformed("program header offset is too large"))?;
    let phnum = u16_at(56) as usize;
    if phnum == 0
        || phnum > 128
        || phoff > bytes.len()
        || phnum
            .checked_mul(56)
            .is_none_or(|n| n > bytes.len() - phoff)
    {
        return Err(malformed("program header table is out of bounds"));
    }
    let mut entry_executable = false;
    let mut has_load = false;
    for index in 0..phnum {
        let offset = phoff + index * 56;
        let kind = u32_at(offset);
        let flags = u32_at(offset + 4);
        if kind == 2 || kind == 3 {
            return Err(malformed(
                "dynamic linking and interpreters are not supported",
            ));
        }
        if kind == 0x6474_e551 && flags & 1 != 0 {
            return Err(malformed("executable stack requested"));
        }
        if kind != 1 {
            continue;
        }
        has_load = true;
        let file_offset = u64_at(offset + 8);
        let virtual_address = u64_at(offset + 16);
        let file_size = u64_at(offset + 32);
        let memory_size = u64_at(offset + 40);
        let alignment = u64_at(offset + 48);
        if file_size > memory_size
            || file_offset > bytes.len() as u64
            || file_size > bytes.len() as u64 - file_offset
            || !(0x10000..0x0000_8000_0000_0000).contains(&virtual_address)
            || memory_size > 0x0000_8000_0000_0000 - virtual_address
            || flags & 3 == 3
            || (alignment > 1
                && (alignment & (alignment - 1) != 0
                    || virtual_address % alignment != file_offset % alignment))
        {
            return Err(malformed("unsafe or out-of-bounds PT_LOAD segment"));
        }
        if flags & 1 != 0
            && entry >= virtual_address
            && entry < virtual_address.saturating_add(memory_size)
        {
            entry_executable = true;
        }
    }
    if !has_load || !entry_executable {
        return Err(malformed(
            "entry point is not in an executable load segment",
        ));
    }
    Ok(())
}

pub fn validate_package_name(value: &str) -> Result<()> {
    if value.is_empty()
        || value.len() > 64
        || value.starts_with('-')
        || !value.bytes().all(|c| {
            c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-' || c == b'_' || c == b'.'
        })
    {
        return Err(ApmError::InvalidManifest(format!(
            "invalid package name {value:?}"
        )));
    }
    Ok(())
}

pub fn validate_destination(value: &str) -> Result<()> {
    if !value.starts_with('/')
        || value.starts_with("//")
        || value.contains('\\')
        || value.contains('\0')
    {
        return Err(ApmError::InvalidManifest(format!(
            "unsafe installation destination {value:?}"
        )));
    }
    let mut parts = value.split('/');
    let _root = parts.next();
    let first = parts.next().unwrap_or_default();
    if first != "usr" {
        return Err(ApmError::InvalidManifest(format!(
            "destination is outside approved /usr roots: {value}"
        )));
    }
    for part in parts {
        if part.is_empty() || part == "." || part == ".." {
            return Err(ApmError::InvalidManifest(format!(
                "unsafe destination component in {value:?}"
            )));
        }
    }
    let path = Path::new(value);
    if path.components().any(|c| {
        matches!(
            c,
            Component::ParentDir | Component::CurDir | Component::Prefix(_)
        )
    }) {
        return Err(ApmError::InvalidManifest(format!(
            "unsafe destination path {value:?}"
        )));
    }
    if !["/usr/bin/", "/usr/lib/", "/usr/share/", "/usr/include/"]
        .iter()
        .any(|root| value.starts_with(root))
    {
        return Err(ApmError::InvalidManifest(format!(
            "destination must be under /usr/bin, /usr/lib, /usr/share, or /usr/include: {value}"
        )));
    }
    Ok(())
}

pub fn validate_manifest(m: &Manifest, packaged: bool) -> Result<()> {
    if m.format_version != FORMAT_VERSION {
        return Err(ApmError::InvalidManifest(format!(
            "unsupported format version {}",
            m.format_version
        )));
    }
    validate_package_name(&m.package.name)?;
    semver::Version::parse(&m.package.version)
        .map_err(|e| ApmError::InvalidManifest(format!("invalid semantic version: {e}")))?;
    if m.package.release == 0 {
        return Err(ApmError::InvalidManifest(
            "release must be at least 1".into(),
        ));
    }
    if m.package.arch != "x86_64" {
        return Err(ApmError::InvalidManifest(format!(
            "unsupported architecture {:?}",
            m.package.arch
        )));
    }
    if m.package.abi != "anix-1" {
        return Err(ApmError::InvalidManifest(format!(
            "unsupported ABI {:?}",
            m.package.abi
        )));
    }
    if m.package.description.trim().is_empty() || m.package.license.trim().is_empty() {
        return Err(ApmError::InvalidManifest(
            "description and license are required".into(),
        ));
    }
    for (name, requirement) in m.dependencies.iter().chain(m.optional_dependencies.iter()) {
        validate_package_name(name)?;
        if name == &m.package.name {
            return Err(ApmError::InvalidManifest(
                "package cannot depend on itself".into(),
            ));
        }
        semver::VersionReq::parse(requirement).map_err(|e| {
            ApmError::InvalidManifest(format!("invalid requirement for {name}: {e}"))
        })?;
    }
    let mut destinations = BTreeSet::new();
    let mut archive_paths = BTreeSet::new();
    if m.files.len() > MAX_PACKAGE_FILES {
        return Err(ApmError::InvalidManifest("too many package files".into()));
    }
    for f in &m.files {
        validate_destination(&f.destination)?;
        if !destinations.insert(&f.destination) {
            return Err(ApmError::InvalidManifest(format!(
                "duplicate destination {}",
                f.destination
            )));
        }
        if packaged {
            if !f.archive_path.starts_with("payload/")
                || !safe_relative_path(&f.archive_path)
                || !archive_paths.insert(&f.archive_path)
            {
                return Err(ApmError::InvalidManifest(format!(
                    "unsafe or duplicate archive path {:?}",
                    f.archive_path
                )));
            }
            if f.sha256.len() != 64 || !f.sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err(ApmError::InvalidManifest(format!(
                    "invalid SHA-256 for {}",
                    f.destination
                )));
            }
        }
        if f.mode & !0o755 != 0 || f.mode & 0o022 != 0 {
            return Err(ApmError::InvalidManifest(format!(
                "unsafe file mode {:o} for {}",
                f.mode, f.destination
            )));
        }
        if packaged && f.source.is_some() {
            return Err(ApmError::InvalidManifest(
                "source fields are only allowed in package definitions".into(),
            ));
        }
    }
    for name in &m.conflicts {
        validate_package_name(name)?;
    }
    for name in &m.provides {
        validate_package_name(name)?;
    }
    Ok(())
}

fn safe_relative_path(value: &str) -> bool {
    if value.is_empty() || value.starts_with('/') || value.contains('\\') {
        return false;
    }
    Path::new(value)
        .components()
        .all(|c| matches!(c, Component::Normal(_)))
}

pub fn read_definition(path: &Path) -> Result<Manifest> {
    let bytes = fs::read(path)?;
    let text = std::str::from_utf8(&bytes).map_err(|e| ApmError::InvalidManifest(e.to_string()))?;
    let manifest: Manifest =
        toml::from_str(text).map_err(|e| ApmError::InvalidManifest(e.to_string()))?;
    validate_manifest(&manifest, false)?;
    for file in &manifest.files {
        if file
            .source
            .as_deref()
            .is_none_or(|s| !safe_relative_path(s))
        {
            return Err(ApmError::InvalidManifest(format!(
                "{} needs a safe relative source path",
                file.destination
            )));
        }
    }
    Ok(manifest)
}

pub fn build_package(definition_path: &Path, output_path: &Path) -> Result<Manifest> {
    let source_root = definition_path
        .parent()
        .ok_or_else(|| ApmError::InvalidManifest("definition has no parent directory".into()))?
        .canonicalize()?;
    let definition = read_definition(definition_path)?;
    let mut output_manifest = definition.clone();
    let mut files: Vec<(PackageFile, PathBuf)> = Vec::new();
    for (index, entry) in definition.files.iter().enumerate() {
        let source = entry.source.as_deref().unwrap();
        let source_path = source_root.join(source);
        let mut source_component = source_root.clone();
        for component in Path::new(source).components() {
            source_component.push(component.as_os_str());
            if fs::symlink_metadata(&source_component)?
                .file_type()
                .is_symlink()
            {
                return Err(ApmError::InvalidPackage(format!(
                    "symlink source path component is not supported: {source}"
                )));
            }
        }
        let canonical = source_path
            .canonicalize()
            .map_err(|e| ApmError::InvalidPackage(format!("cannot read source {source}: {e}")))?;
        if !canonical.starts_with(&source_root) || !canonical.is_file() {
            return Err(ApmError::InvalidPackage(format!(
                "source escapes package directory or is not a regular file: {source}"
            )));
        }
        let metadata = fs::symlink_metadata(&source_path)?;
        if metadata.file_type().is_symlink() {
            return Err(ApmError::InvalidPackage(format!(
                "symlink package source is not supported: {source}"
            )));
        }
        if metadata.len() > MAX_PACKAGE_BYTES {
            return Err(ApmError::InvalidPackage(format!(
                "source file is too large: {source}"
            )));
        }
        let content = fs::read(&canonical)?;
        let mode = if entry.mode & 0o111 != 0 {
            0o755
        } else {
            0o644
        };
        let archive_path = format!("payload/{index:08}");
        let mut file = entry.clone();
        file.archive_path = archive_path;
        file.sha256 = sha256(&content);
        file.mode = mode;
        file.source = None;
        output_manifest.files[index] = file.clone();
        files.push((file, canonical));
    }
    validate_manifest(&output_manifest, true)?;
    let manifest_bytes = toml::to_string_pretty(&output_manifest)
        .map_err(|e| ApmError::Serialization(e.to_string()))?;
    if let Some(parent) = output_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let output = fs::File::create(output_path)?;
    let encoder = zstd::Encoder::new(output, 10)?;
    let mut archive = tar::Builder::new(encoder);
    archive.mode(tar::HeaderMode::Deterministic);
    append_tar_file(
        &mut archive,
        ARCHIVE_MANIFEST,
        manifest_bytes.as_bytes(),
        0o644,
    )?;
    for (entry, path) in &files {
        let bytes = fs::read(path)?;
        append_tar_file(&mut archive, &entry.archive_path, &bytes, entry.mode)?;
    }
    let encoder = archive.into_inner()?;
    encoder.finish()?;
    verify_package(output_path)?;
    Ok(output_manifest)
}

fn append_tar_file<W: Write>(
    archive: &mut tar::Builder<W>,
    name: &str,
    bytes: &[u8],
    mode: u32,
) -> Result<()> {
    let mut header = tar::Header::new_gnu();
    header.set_size(bytes.len() as u64);
    header.set_mode(mode);
    header.set_uid(0);
    header.set_gid(0);
    header.set_mtime(0);
    header.set_entry_type(tar::EntryType::Regular);
    header.set_cksum();
    archive.append_data(&mut header, name, bytes)?;
    Ok(())
}

pub fn verify_package(path: &Path) -> Result<VerifiedPackage> {
    if fs::metadata(path)?.len() > MAX_PACKAGE_BYTES {
        return Err(ApmError::InvalidPackage(
            "compressed package exceeds size limit".into(),
        ));
    }
    let input = fs::File::open(path)?;
    let decoder = zstd::Decoder::new(input).map_err(|e| {
        ApmError::InvalidPackage(format!("not a valid zstd-compressed package: {e}"))
    })?;
    let mut archive = tar::Archive::new(decoder);
    let mut manifest: Option<Manifest> = None;
    let mut payload = BTreeMap::new();
    let mut total: u64 = 0;
    let entries = archive
        .entries()
        .map_err(|e| ApmError::InvalidPackage(e.to_string()))?;
    for entry in entries {
        let mut entry = entry.map_err(|e| ApmError::InvalidPackage(e.to_string()))?;
        let path = entry
            .path()
            .map_err(|e| ApmError::InvalidPackage(e.to_string()))?
            .to_string_lossy()
            .to_string();
        if !safe_relative_path(&path) || !entry.header().entry_type().is_file() {
            return Err(ApmError::InvalidPackage(format!(
                "unsafe or non-regular archive member: {path}"
            )));
        }
        if manifest.is_none() && path != ARCHIVE_MANIFEST {
            return Err(ApmError::InvalidPackage(
                "manifest.toml must be the first archive member".into(),
            ));
        }
        let size = entry.size();
        total = total
            .checked_add(size)
            .ok_or_else(|| ApmError::InvalidPackage("archive size overflow".into()))?;
        if total > MAX_PACKAGE_BYTES {
            return Err(ApmError::InvalidPackage(
                "uncompressed package exceeds size limit".into(),
            ));
        }
        let mut bytes = Vec::with_capacity(size.min(1024 * 1024) as usize);
        entry.read_to_end(&mut bytes)?;
        if path == ARCHIVE_MANIFEST {
            if manifest.is_some() {
                return Err(ApmError::InvalidPackage("duplicate manifest.toml".into()));
            }
            let text =
                std::str::from_utf8(&bytes).map_err(|e| ApmError::InvalidPackage(e.to_string()))?;
            let parsed: Manifest = toml::from_str(text)
                .map_err(|e| ApmError::InvalidPackage(format!("invalid manifest: {e}")))?;
            validate_manifest(&parsed, true)?;
            manifest = Some(parsed);
        } else if path.starts_with("payload/") {
            let mode = entry
                .header()
                .mode()
                .map_err(|e| ApmError::InvalidPackage(e.to_string()))?
                & 0o777;
            if payload.insert(path.clone(), bytes).is_some() {
                return Err(ApmError::InvalidPackage(format!(
                    "duplicate archive member: {path}"
                )));
            }
            let record = manifest
                .as_ref()
                .and_then(|m| m.files.iter().find(|f| f.archive_path == path));
            if let Some(file) = record {
                if file.mode != mode {
                    return Err(ApmError::InvalidPackage(format!(
                        "mode mismatch for {}",
                        file.destination
                    )));
                }
            }
        } else {
            return Err(ApmError::InvalidPackage(format!(
                "unexpected archive member: {path}"
            )));
        }
    }
    let manifest =
        manifest.ok_or_else(|| ApmError::InvalidPackage("missing manifest.toml".into()))?;
    if payload.len() != manifest.files.len() {
        return Err(ApmError::InvalidPackage(
            "payload file count does not match manifest".into(),
        ));
    }
    for file in &manifest.files {
        let bytes = payload.get(&file.archive_path).ok_or_else(|| {
            ApmError::InvalidPackage(format!("missing payload {}", file.archive_path))
        })?;
        if sha256(bytes) != file.sha256.to_ascii_lowercase() {
            return Err(ApmError::InvalidPackage(format!(
                "hash mismatch for {}",
                file.destination
            )));
        }
        if file.destination.starts_with("/usr/bin/") {
            validate_anix_elf(bytes)?;
        }
    }
    if payload
        .keys()
        .any(|key| !manifest.files.iter().any(|f| &f.archive_path == key))
    {
        return Err(ApmError::InvalidPackage("unlisted payload member".into()));
    }
    Ok(VerifiedPackage { manifest, payload })
}
