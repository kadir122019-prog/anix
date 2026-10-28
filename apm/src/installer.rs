use crate::{
    database::{self, Database, InstalledPackage},
    package::{sha256, validate_destination, validate_manifest, VerifiedPackage},
    ApmError, Result,
};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::{Path, PathBuf},
    time::UNIX_EPOCH,
};

#[derive(Serialize, Deserialize)]
struct Journal {
    version: u32,
    state: String,
    database_existed: bool,
    files: Vec<JournalFile>,
}
#[derive(Serialize, Deserialize)]
struct JournalFile {
    destination: String,
    had_original: bool,
    new_index: Option<usize>,
}

struct Lock {
    path: PathBuf,
}
impl Drop for Lock {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

fn create_transaction_root(root: &Path, db_path: &Path) -> Result<(String, PathBuf)> {
    let transactions = db_path.parent().unwrap().join("transactions");
    match fs::symlink_metadata(&transactions) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            return Err(ApmError::Transaction(
                "transaction directory must be a real directory".into(),
            ))
        }
        Ok(_) => (),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            #[cfg(unix)]
            {
                use std::os::unix::fs::DirBuilderExt;
                let mut builder = fs::DirBuilder::new();
                builder.mode(0o700);
                builder.create(&transactions)?;
            }
            #[cfg(not(unix))]
            {
                fs::create_dir(&transactions)?;
            }
        }
        Err(error) => return Err(error.into()),
    }
    if !transactions.canonicalize()?.starts_with(root) {
        return Err(ApmError::Transaction(
            "transaction directory escapes installation root".into(),
        ));
    }
    for _ in 0..8 {
        let mut random = [0u8; 16];
        getrandom::fill(&mut random)
            .map_err(|e| ApmError::Transaction(format!("secure random source failed: {e}")))?;
        let id = hex::encode(random);
        let path = transactions.join(&id);
        #[cfg(unix)]
        {
            use std::os::unix::fs::DirBuilderExt;
            let mut builder = fs::DirBuilder::new();
            builder.mode(0o700);
            match builder.create(&path) {
                Ok(()) => return Ok((id, path)),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e.into()),
            }
        }
        #[cfg(not(unix))]
        {
            match fs::create_dir(&path) {
                Ok(()) => return Ok((id, path)),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e.into()),
            }
        }
    }
    Err(ApmError::Transaction(
        "could not allocate unique transaction directory".into(),
    ))
}

fn acquire_lock(db_path: &Path) -> Result<Lock> {
    let parent = db_path
        .parent()
        .ok_or_else(|| ApmError::Transaction("database path has no parent".into()))?;
    fs::create_dir_all(parent)?;
    let path = parent.join("apm.lock");
    fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .map_err(|e| {
            ApmError::Transaction(format!(
                "could not lock package database {}: {e}",
                path.display()
            ))
        })?;
    Ok(Lock { path })
}

fn root_path(root: &Path) -> Result<PathBuf> {
    fs::create_dir_all(root)?;
    let canonical = root.canonicalize()?;
    if canonical.parent().is_none() {
        return Err(ApmError::Transaction(
            "refusing to use a filesystem root as the package installation root".into(),
        ));
    }
    Ok(canonical)
}

fn target_path(root: &Path, destination: &str) -> Result<PathBuf> {
    validate_destination(destination)?;
    let relative = destination.trim_start_matches('/');
    let target = root.join(relative);
    let mut cursor = root.to_path_buf();
    let parts: Vec<_> = Path::new(relative).components().collect();
    for part in parts.iter().take(parts.len().saturating_sub(1)) {
        cursor.push(part.as_os_str());
        match fs::symlink_metadata(&cursor) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Err(ApmError::Transaction(format!(
                    "refusing symlinked path component {}",
                    cursor.display()
                )))
            }
            Ok(metadata) if !metadata.is_dir() => {
                return Err(ApmError::Transaction(format!(
                    "path component is not a directory: {}",
                    cursor.display()
                )))
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => (),
            Err(error) => return Err(error.into()),
            _ => (),
        }
    }
    if let Ok(metadata) = fs::symlink_metadata(&target) {
        if metadata.file_type().is_symlink() || !metadata.is_file() {
            return Err(ApmError::Transaction(format!(
                "destination is not a regular file: {}",
                target.display()
            )));
        }
    }
    Ok(target)
}

fn db_relative(root: &Path, db_path: &Path) -> Result<PathBuf> {
    let absolute = if db_path.is_absolute() {
        db_path.to_path_buf()
    } else {
        root.join(db_path)
    };
    if let Some(parent) = absolute.parent() {
        fs::create_dir_all(parent)?;
    }
    let parent = absolute.parent().unwrap().canonicalize()?;
    let root = root.canonicalize()?;
    if !parent.starts_with(&root) {
        return Err(ApmError::Transaction(
            "package database must live inside the installation root".into(),
        ));
    }
    if absolute.exists() && fs::symlink_metadata(&absolute)?.file_type().is_symlink() {
        return Err(ApmError::Transaction(
            "refusing a symlinked package database".into(),
        ));
    }
    Ok(parent.join(
        absolute
            .file_name()
            .ok_or_else(|| ApmError::Transaction("invalid database filename".into()))?,
    ))
}

fn check_owned_file(
    root: &Path,
    manifest: &crate::package::Manifest,
    destination: &str,
) -> Result<bool> {
    let Some(file) = manifest
        .files
        .iter()
        .find(|file| file.destination == destination)
    else {
        return Ok(false);
    };
    let target = target_path(root, destination)?;
    match fs::read(target) {
        Ok(bytes) if sha256(&bytes) == file.sha256 => Ok(true),
        Ok(_) => Err(ApmError::Transaction(format!(
            "locally modified package-owned file: {destination}"
        ))),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error.into()),
    }
}

pub fn install(
    root: &Path,
    db_path: &Path,
    packages: &[VerifiedPackage],
    source: &str,
) -> Result<String> {
    if packages.is_empty() {
        return Err(ApmError::Transaction("empty install transaction".into()));
    }
    let root = root_path(root)?;
    let db_path = db_relative(&root, db_path)?;
    let _lock = acquire_lock(&db_path)?;
    let mut database = Database::load(&db_path)?;
    let (tx, tx_root) = create_transaction_root(&root, &db_path)?;
    let stage = tx_root.join("new");
    let backups = tx_root.join("backup");
    fs::create_dir_all(&stage)?;
    fs::create_dir_all(&backups)?;
    let database_existed = db_path.exists();
    if database_existed {
        fs::copy(&db_path, tx_root.join("database-old.json"))?;
    }

    let mut new_manifests = BTreeMap::new();
    let mut staged_files = Vec::<(String, usize, Vec<u8>)>::new();
    for package in packages {
        validate_manifest(&package.manifest, true)?;
        let name = &package.manifest.package.name;
        if new_manifests
            .insert(name.clone(), package.manifest.clone())
            .is_some()
        {
            return Err(ApmError::Transaction(format!(
                "duplicate package in plan: {name}"
            )));
        }
        for file in &package.manifest.files {
            let bytes = package.payload.get(&file.archive_path).ok_or_else(|| {
                ApmError::Transaction(format!(
                    "verified payload disappeared: {}",
                    file.archive_path
                ))
            })?;
            if sha256(bytes) != file.sha256.to_ascii_lowercase() {
                return Err(ApmError::InvalidPackage(format!(
                    "payload hash mismatch for {}",
                    file.destination
                )));
            }
            if file.destination.starts_with("/usr/bin/") {
                crate::package::validate_anix_elf(bytes)?;
            }
            let index = staged_files.len();
            let staged_path = stage.join(index.to_string());
            fs::write(&staged_path, bytes)?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                fs::set_permissions(&staged_path, fs::Permissions::from_mode(file.mode))?;
            }
            staged_files.push((file.destination.clone(), index, bytes.clone()));
        }
    }
    let mut incoming = BTreeSet::new();
    for (destination, _, _) in &staged_files {
        if !incoming.insert(destination.clone()) {
            return Err(ApmError::Transaction(format!(
                "two packages claim {destination}"
            )));
        }
    }
    let mut journal_files = Vec::new();
    for (destination, index, _) in &staged_files {
        let target = target_path(&root, destination)?;
        let mut owner: Option<&InstalledPackage> = None;
        for installed in database.packages.values() {
            if installed
                .manifest
                .files
                .iter()
                .any(|f| f.destination == *destination)
            {
                owner = Some(installed);
                break;
            }
        }
        match (target.exists(), owner) {
            (true, Some(owner)) => {
                check_owned_file(&root, &owner.manifest, destination)?;
            }
            (true, None) => {
                return Err(ApmError::Transaction(format!(
                    "refusing to overwrite unowned file {destination}"
                )))
            }
            (false, Some(owner)) => {
                let _ = owner;
            }
            (false, None) => (),
        }
        if let Some(owner) = owner {
            if !new_manifests
                .get(&owner.manifest.package.name)
                .is_some_and(|m| m.files.iter().any(|file| file.destination == *destination))
            {
                return Err(ApmError::Transaction(format!(
                    "file {destination} is owned by {}",
                    owner.manifest.package.name
                )));
            }
        }
        journal_files.push(JournalFile {
            destination: destination.clone(),
            had_original: target.exists(),
            new_index: Some(*index),
        });
    }
    // Old files omitted by an upgrade are removed in the same recoverable transaction.
    for (name, old) in &database.packages {
        if let Some(new) = new_manifests.get(name) {
            for file in &old.manifest.files {
                if !new
                    .files
                    .iter()
                    .any(|nf| nf.destination == file.destination)
                    && !incoming.contains(&file.destination)
                {
                    let target = target_path(&root, &file.destination)?;
                    if target.exists() {
                        check_owned_file(&root, &old.manifest, &file.destination)?;
                    }
                    journal_files.push(JournalFile {
                        destination: file.destination.clone(),
                        had_original: target.exists(),
                        new_index: None,
                    });
                }
            }
        }
    }
    let mut journal = Journal {
        version: 1,
        state: "prepared".into(),
        database_existed,
        files: journal_files,
    };
    save_journal(&tx_root, &journal)?;
    journal.state = "committing".into();
    save_journal(&tx_root, &journal)?;
    let operation = (|| -> Result<()> {
        for (index, file) in journal.files.iter().enumerate() {
            let target = target_path(&root, &file.destination)?;
            fs::create_dir_all(target.parent().unwrap())?;
            if target.exists() {
                fs::rename(&target, backups.join(index.to_string()))?;
            }
            if let Some(new_index) = file.new_index {
                fs::rename(stage.join(new_index.to_string()), &target)?;
            }
        }
        for (name, manifest) in new_manifests {
            let version = manifest.package.version.clone();
            database.packages.insert(
                name.clone(),
                InstalledPackage {
                    manifest,
                    installed_at: database::now(),
                    source: source.to_string(),
                },
            );
            database.add_history("install", &name, &version, &tx);
        }
        database.save(&db_path)?;
        journal.state = "committed".into();
        save_journal(&tx_root, &journal)?;
        Ok(())
    })();
    if let Err(error) = operation {
        let recovery = rollback(&root, &db_path, &tx_root, &journal);
        if recovery.is_ok() {
            let _ = fs::remove_dir_all(&tx_root);
        }
        return Err(recovery.err().unwrap_or(error));
    }
    fs::remove_dir_all(&tx_root)?;
    Ok(tx)
}

pub fn remove(root: &Path, db_path: &Path, name: &str) -> Result<String> {
    remove_many(root, db_path, &[name.to_string()])
}

pub fn remove_many(root: &Path, db_path: &Path, names: &[String]) -> Result<String> {
    if names.is_empty() {
        return Err(ApmError::Transaction("empty remove transaction".into()));
    }
    let root = root_path(root)?;
    let db_path = db_relative(&root, db_path)?;
    let _lock = acquire_lock(&db_path)?;
    let mut database = Database::load(&db_path)?;
    let mut installed = BTreeMap::new();
    for name in names {
        if installed.contains_key(name) {
            continue;
        }
        let package = database
            .packages
            .get(name)
            .ok_or_else(|| ApmError::Transaction(format!("package {name} is not installed")))?
            .clone();
        installed.insert(name.clone(), package);
    }
    let (tx, tx_root) = create_transaction_root(&root, &db_path)?;
    let backups = tx_root.join("backup");
    fs::create_dir_all(&backups)?;
    let database_existed = db_path.exists();
    if database_existed {
        fs::copy(&db_path, tx_root.join("database-old.json"))?;
    }
    let mut files = Vec::new();
    let mut destinations = BTreeSet::new();
    for package in installed.values() {
        for file in &package.manifest.files {
            if !destinations.insert(file.destination.clone()) {
                return Err(ApmError::Transaction(format!(
                    "multiple packages claim {}",
                    file.destination
                )));
            }
            let target = target_path(&root, &file.destination)?;
            if target.exists() {
                check_owned_file(&root, &package.manifest, &file.destination)?;
            }
            files.push(JournalFile {
                destination: file.destination.clone(),
                had_original: target.exists(),
                new_index: None,
            });
        }
    }
    let mut journal = Journal {
        version: 1,
        state: "prepared".into(),
        database_existed,
        files,
    };
    save_journal(&tx_root, &journal)?;
    journal.state = "committing".into();
    save_journal(&tx_root, &journal)?;
    let operation = (|| -> Result<()> {
        for (index, file) in journal.files.iter().enumerate() {
            if file.had_original {
                let target = target_path(&root, &file.destination)?;
                fs::rename(target, backups.join(index.to_string()))?;
            }
        }
        for (name, package) in &installed {
            database.packages.remove(name);
            database.add_history("remove", name, &package.manifest.package.version, &tx);
        }
        database.save(&db_path)?;
        journal.state = "committed".into();
        save_journal(&tx_root, &journal)?;
        Ok(())
    })();
    if let Err(error) = operation {
        let recovery = rollback(&root, &db_path, &tx_root, &journal);
        if recovery.is_ok() {
            let _ = fs::remove_dir_all(&tx_root);
        }
        return Err(recovery.err().unwrap_or(error));
    }
    fs::remove_dir_all(&tx_root)?;
    Ok(tx)
}

fn save_journal(tx_root: &Path, journal: &Journal) -> Result<()> {
    let bytes =
        serde_json::to_vec_pretty(journal).map_err(|e| ApmError::Serialization(e.to_string()))?;
    database::atomic_write(&tx_root.join("journal.json"), &bytes)
}

fn rollback(root: &Path, db_path: &Path, tx_root: &Path, journal: &Journal) -> Result<()> {
    for (index, file) in journal.files.iter().enumerate().rev() {
        let target = target_path(root, &file.destination)?;
        let backup = tx_root.join("backup").join(index.to_string());
        let staged_exists = file
            .new_index
            .is_some_and(|n| tx_root.join("new").join(n.to_string()).exists());
        if backup.exists() {
            if target.exists() {
                fs::remove_file(&target)?;
            }
            fs::create_dir_all(target.parent().unwrap())?;
            fs::rename(backup, target)?;
        } else if !file.had_original
            && file.new_index.is_some()
            && !staged_exists
            && target.exists()
        {
            fs::remove_file(target)?;
        }
    }
    let old_db = tx_root.join("database-old.json");
    if journal.database_existed {
        if old_db.exists() {
            let previous = fs::read(old_db)?;
            database::atomic_write(db_path, &previous)?;
        }
    } else if db_path.exists() {
        fs::remove_file(db_path)?;
    }
    Ok(())
}

pub fn recover(root: &Path, db_path: &Path) -> Result<usize> {
    let root = root_path(root)?;
    let db_path = db_relative(&root, db_path)?;
    let _lock = acquire_lock(&db_path)?;
    let tx_parent = db_path.parent().unwrap().join("transactions");
    let mut recovered = 0;
    if !tx_parent.exists() {
        return Ok(0);
    }
    let metadata = fs::symlink_metadata(&tx_parent)?;
    if metadata.file_type().is_symlink()
        || !metadata.is_dir()
        || !tx_parent.canonicalize()?.starts_with(&root)
    {
        return Err(ApmError::Transaction(
            "unsafe transaction recovery directory".into(),
        ));
    }
    for entry in fs::read_dir(&tx_parent)? {
        let entry = entry?;
        if !entry.file_type()?.is_dir() {
            continue;
        }
        let tx_root = entry.path();
        let journal_bytes = match fs::read(tx_root.join("journal.json")) {
            Ok(bytes) => bytes,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                fs::remove_dir_all(tx_root)?;
                recovered += 1;
                continue;
            }
            Err(error) => return Err(error.into()),
        };
        let journal: Journal = serde_json::from_slice(&journal_bytes)
            .map_err(|e| ApmError::Transaction(format!("invalid recovery journal: {e}")))?;
        if journal.version != 1 {
            return Err(ApmError::Transaction("unsupported journal version".into()));
        }
        if journal.state != "committed" {
            rollback(&root, &db_path, &tx_root, &journal)?;
        }
        fs::remove_dir_all(tx_root)?;
        recovered += 1;
    }
    Ok(recovered)
}

pub fn check_database_files(root: &Path, db: &Database) -> Result<Vec<String>> {
    let root = root.canonicalize()?;
    let mut issues = Vec::new();
    for package in db.packages.values() {
        for file in &package.manifest.files {
            let path = target_path(&root, &file.destination)?;
            match fs::read(path) {
                Ok(bytes) if sha256(&bytes) == file.sha256 => (),
                Ok(_) => issues.push(format!("modified: {}", file.destination)),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    issues.push(format!("missing: {}", file.destination))
                }
                Err(error) => return Err(error.into()),
            }
        }
    }
    Ok(issues)
}

pub fn installed_versions(db: &Database) -> BTreeMap<String, String> {
    db.packages
        .iter()
        .map(|(name, package)| (name.clone(), package.manifest.package.version.clone()))
        .collect()
}

#[allow(dead_code)]
fn _mtime(path: &Path) -> Option<u64> {
    fs::metadata(path)
        .ok()?
        .modified()
        .ok()?
        .duration_since(UNIX_EPOCH)
        .ok()
        .map(|v| v.as_secs())
}
