use apm::package::build_package;
use clap::Parser;
use std::path::PathBuf;

#[derive(Parser)]
#[command(
    name = "apkg-build",
    version,
    about = "Build a deterministic Anix .apkg archive"
)]
struct Args {
    definition: PathBuf,
    output: PathBuf,
}

fn main() {
    let args = Args::parse();
    match build_package(&args.definition, &args.output) {
        Ok(manifest) => println!(
            "built {} {}-{} for {} ({}) -> {}",
            manifest.package.name,
            manifest.package.version,
            manifest.package.release,
            manifest.package.arch,
            manifest.package.abi,
            args.output.display()
        ),
        Err(error) => {
            eprintln!("apkg-build: {error}");
            std::process::exit(1);
        }
    }
}
