use clap::Parser;
use ed25519_dalek::SigningKey;
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::PathBuf,
};

#[derive(Parser)]
#[command(
    name = "apkg-keygen",
    version,
    about = "Create an Ed25519 key for signing Anix repository metadata"
)]
struct Args {
    #[arg(long)]
    private: PathBuf,
    #[arg(long)]
    public: PathBuf,
}

fn main() {
    let args = Args::parse();
    let mut seed = [0u8; 32];
    if let Err(error) = getrandom::fill(&mut seed) {
        eprintln!("apkg-keygen: secure random source failed: {error}");
        std::process::exit(1);
    }
    let key = SigningKey::from_bytes(&seed);
    let private_text = hex::encode(seed);
    let public_text = hex::encode(key.verifying_key().as_bytes());
    if let Some(parent) = args.private.parent() {
        if let Err(e) = fs::create_dir_all(parent) {
            fail(e);
        }
    }
    if let Some(parent) = args.public.parent() {
        if let Err(e) = fs::create_dir_all(parent) {
            fail(e);
        }
    }
    let mut private_options = OpenOptions::new();
    private_options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        private_options.mode(0o600);
    }
    let mut private = match private_options.open(&args.private) {
        Ok(f) => f,
        Err(e) => fail(e),
    };
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if private
            .metadata()
            .map(|m| m.permissions().mode() & 0o077 != 0)
            .unwrap_or(true)
        {
            drop(private);
            let _ = fs::remove_file(&args.private);
            fail("filesystem does not enforce owner-only private-key permissions; choose a private local filesystem");
        }
    }
    if let Err(e) = private
        .write_all(private_text.as_bytes())
        .and_then(|_| private.write_all(b"\n"))
        .and_then(|_| private.sync_all())
    {
        fail(e);
    }
    if let Err(e) = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&args.public)
        .and_then(|mut f| f.write_all(format!("{public_text}\n").as_bytes()))
    {
        fail(e);
    }
    println!("created Ed25519 signing key; trusted public key: {public_text}");
    eprintln!(
        "keep {} secret and backed up; it cannot be recovered from the public key",
        args.private.display()
    );
}

fn fail(error: impl std::fmt::Display) -> ! {
    eprintln!("apkg-keygen: {error}");
    std::process::exit(1)
}
