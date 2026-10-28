pub mod database;
pub mod installer;
pub mod package;
pub mod repository;
pub mod resolver;

use thiserror::Error;

#[derive(Debug, Error)]
pub enum ApmError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("invalid package: {0}")]
    InvalidPackage(String),
    #[error("invalid manifest: {0}")]
    InvalidManifest(String),
    #[error("database error: {0}")]
    Database(String),
    #[error("transaction error: {0}")]
    Transaction(String),
    #[error("dependency resolution failed: {0}")]
    Resolution(String),
    #[error("serialization error: {0}")]
    Serialization(String),
}

pub type Result<T> = std::result::Result<T, ApmError>;
