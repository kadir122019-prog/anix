use apm::{
    repository::{self, signing_key_from_hex_file},
    ApmError,
};
use clap::Parser;
use std::{path::PathBuf, time::Duration};

#[derive(Parser)]
#[command(
    name = "apkg-repo-sign",
    version,
    about = "Sign local Anix repository package metadata"
)]
struct Args {
    #[arg(long)]
    directory: PathBuf,
    #[arg(long)]
    key: PathBuf,
    #[arg(long = "metadata-version")]
    metadata_version: u64,
    #[arg(long, default_value_t = 24)]
    expires_hours: u64,
}

fn main() {
    let args = Args::parse();
    let result = (|| -> apm::Result<()> {
        if args.expires_hours == 0 || args.expires_hours > 24 * 30 {
            return Err(ApmError::Transaction(
                "expiry must be between 1 hour and 30 days".into(),
            ));
        }
        let key = signing_key_from_hex_file(&args.key)?;
        let expiry = repository::now()
            .saturating_add(Duration::from_secs(args.expires_hours * 3600).as_secs());
        let document =
            repository::sign_directory(&args.directory, args.metadata_version, expiry, &key)?;
        apm::database::atomic_write(&args.directory.join("repository.json"), &document)?;
        println!(
            "signed repository metadata v{} (expires in {} hours)",
            args.metadata_version, args.expires_hours
        );
        Ok(())
    })();
    if let Err(error) = result {
        eprintln!("apkg-repo-sign: {error}");
        std::process::exit(1);
    }
}
