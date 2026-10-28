use crate::{package::Manifest, ApmError, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct InstalledPackage {
    pub manifest: Manifest,
    pub installed_at: u64,
    pub source: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct HistoryEntry {
    pub timestamp: u64,
    pub action: String,
    pub package: String,
    pub version: String,
    pub transaction: String,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Database {
    pub schema_version: u32,
    pub packages: BTreeMap<String, InstalledPackage>,
    pub history: Vec<HistoryEntry>,
}

impl Database {
    pub fn load(path: &Path) -> Result<Self> {
        match fs::read(path) {
            Ok(bytes) => {
                let db: Self = serde_json::from_slice(&bytes).map_err(|e| {
                    ApmError::Database(format!("{} is corrupt: {e}", path.display()))
                })?;
                if db.schema_version != 1 {
                    return Err(ApmError::Database(format!(
                        "unsupported database schema {}",
                        db.schema_version
                    )));
                }
                Ok(db)
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(Self {
                schema_version: 1,
                ..Self::default()
            }),
            Err(error) => Err(error.into()),
        }
    }

    pub fn save(&self, path: &Path) -> Result<()> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let bytes =
            serde_json::to_vec_pretty(self).map_err(|e| ApmError::Serialization(e.to_string()))?;
        atomic_write(path, &bytes)
    }

    pub fn add_history(&mut self, action: &str, name: &str, version: &str, transaction: &str) {
        self.history.push(HistoryEntry {
            timestamp: now(),
            action: action.into(),
            package: name.into(),
            version: version.into(),
            transaction: transaction.into(),
        });
    }
}

pub fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

pub fn atomic_write(path: &Path, data: &[u8]) -> Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| ApmError::Database("database path has no parent".into()))?;
    fs::create_dir_all(parent)?;
    let tmp = unique_sibling(path, "tmp");
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&tmp)?;
    let result = (|| -> Result<()> {
        file.write_all(data)?;
        file.sync_all()?;
        drop(file);
        fs::rename(&tmp, path)?;
        if let Ok(directory) = fs::File::open(parent) {
            let _ = directory.sync_all();
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

pub fn unique_sibling(path: &Path, suffix: &str) -> PathBuf {
    let file = path.file_name().unwrap_or_default().to_string_lossy();
    path.with_file_name(format!(
        ".{file}.{}.{}.{}",
        std::process::id(),
        now(),
        suffix
    ))
}
