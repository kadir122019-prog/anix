use apm::package::verify_package;
use clap::Parser;
use std::path::PathBuf;

#[derive(Parser)]
#[command(
    name = "apkg-verify",
    version,
    about = "Validate an Anix .apkg archive and every payload hash"
)]
struct Args {
    package: PathBuf,
    #[arg(long)]
    json: bool,
}

fn main() {
    let args = Args::parse();
    match verify_package(&args.package) {
        Ok(package) if args.json => println!(
            "{}",
            serde_json::to_string_pretty(&package.manifest).unwrap()
        ),
        Ok(package) => println!(
            "verified {} {}-{} ({} files, {})",
            package.manifest.package.name,
            package.manifest.package.version,
            package.manifest.package.release,
            package.manifest.files.len(),
            package.manifest.package.abi
        ),
        Err(error) => {
            eprintln!("apkg-verify: {error}");
            std::process::exit(1);
        }
    }
}
