use apm::{
    database::{Database, InstalledPackage},
    installer,
    package::{verify_package, Manifest},
    repository,
    resolver::{self, InstalledSet},
    ApmError, Result,
};
use clap::{Parser, Subcommand};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::{self, Write},
    path::{Path, PathBuf},
};

#[derive(Parser)]
#[command(
    name = "apm",
    version,
    about = "Anix package manager (host-side local and signed HTTPS repositories)"
)]
struct Cli {
    #[arg(
        long,
        global = true,
        default_value = "apm-root",
        help = "Isolated Anix installation root; never defaults to /"
    )]
    root: PathBuf,
    #[arg(
        short = 'y',
        long,
        global = true,
        help = "Do not prompt; accept the displayed local transaction"
    )]
    yes: bool,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Update,
    Search {
        query: String,
    },
    Info {
        package: String,
    },
    Install {
        packages: Vec<String>,
    },
    Remove {
        packages: Vec<String>,
    },
    List,
    Upgrade {
        package: Option<String>,
    },
    Verify {
        package: String,
    },
    Clean,
    History,
    Recover,
    Repo {
        #[command(subcommand)]
        command: RepoCommand,
    },
}

#[derive(Subcommand)]
enum RepoCommand {
    List,
    Add {
        name: String,
        location: String,
        #[arg(long)]
        key: Option<String>,
    },
    Remove {
        name: String,
    },
}

#[derive(Clone, Debug, Serialize, Deserialize, Default)]
struct RepoConfig {
    repositories: BTreeMap<String, RepoSpec>,
    #[serde(default)]
    metadata_versions: BTreeMap<String, u64>,
    #[serde(default)]
    metadata_digests: BTreeMap<String, String>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
struct RepoSpec {
    location: String,
    trusted_key: Option<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
struct CachedPackage {
    repository: String,
    path: PathBuf,
    manifest: Manifest,
    #[serde(default)]
    expected_sha256: Option<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize, Default)]
struct RepoIndex {
    packages: Vec<CachedPackage>,
}

fn db_path(root: &Path) -> PathBuf {
    root.join("var/lib/apm/installed.json")
}
fn repo_config_path(root: &Path) -> PathBuf {
    root.join("etc/apm/repositories.json")
}
fn index_path(root: &Path) -> PathBuf {
    root.join("var/cache/apm/repositories.json")
}

fn prepare_root(path: &Path) -> Result<PathBuf> {
    fs::create_dir_all(path)?;
    let root = path.canonicalize()?;
    if root.parent().is_none() {
        return Err(ApmError::Transaction(
            "refusing the filesystem root; choose a sandbox installation directory".into(),
        ));
    }
    Ok(root)
}

fn safe_state_file(root: &Path, relative: &str) -> Result<PathBuf> {
    let root = root.canonicalize()?;
    let parts: Vec<_> = Path::new(relative).components().collect();
    if parts.is_empty()
        || parts
            .iter()
            .any(|component| !matches!(component, std::path::Component::Normal(_)))
    {
        return Err(ApmError::Transaction("unsafe relative state path".into()));
    }
    let mut cursor = root.clone();
    for component in parts.iter().take(parts.len().saturating_sub(1)) {
        cursor.push(component.as_os_str());
        match fs::symlink_metadata(&cursor) {
            Ok(meta) if meta.file_type().is_symlink() || !meta.is_dir() => {
                return Err(ApmError::Transaction(format!(
                    "unsafe state path component {}",
                    cursor.display()
                )))
            }
            Ok(_) => (),
            Err(error) if error.kind() == io::ErrorKind::NotFound => fs::create_dir(&cursor)?,
            Err(error) => return Err(error.into()),
        }
    }
    if !cursor.canonicalize()?.starts_with(&root) {
        return Err(ApmError::Transaction(
            "state path escapes installation root".into(),
        ));
    }
    let filename = parts
        .last()
        .ok_or_else(|| ApmError::Transaction("empty state path".into()))?;
    cursor.push(filename.as_os_str());
    if cursor.exists() && fs::symlink_metadata(&cursor)?.file_type().is_symlink() {
        return Err(ApmError::Transaction(
            "refusing a symlinked state file".into(),
        ));
    }
    Ok(cursor)
}

fn save_state_json<T: Serialize>(root: &Path, relative: &str, value: &T) -> Result<()> {
    let path = safe_state_file(root, relative)?;
    let data =
        serde_json::to_vec_pretty(value).map_err(|e| ApmError::Serialization(e.to_string()))?;
    apm::database::atomic_write(&path, &data)
}

fn write_cached_package(
    root: &Path,
    repository_name: &str,
    file: &str,
    bytes: &[u8],
) -> Result<PathBuf> {
    if repository_name.is_empty()
        || !repository_name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
        || repository_name == "."
        || repository_name == ".."
    {
        return Err(ApmError::Transaction(
            "unsafe repository name in cache".into(),
        ));
    }
    let relative = format!("var/cache/apm/packages/{repository_name}/{file}");
    let path = safe_state_file(root, &relative)?;
    apm::database::atomic_write(&path, bytes)?;
    Ok(path)
}

fn load_json<T: for<'de> Deserialize<'de> + Default>(path: &Path) -> Result<T> {
    match fs::read(path) {
        Ok(data) => serde_json::from_slice(&data)
            .map_err(|e| ApmError::Serialization(format!("{}: {e}", path.display()))),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(T::default()),
        Err(error) => Err(error.into()),
    }
}
fn load_index(root: &Path) -> Result<RepoIndex> {
    load_json(&index_path(root))
}
fn load_db(root: &Path) -> Result<Database> {
    Database::load(&db_path(root))
}

fn update(root: &Path) -> Result<()> {
    let mut config: RepoConfig = load_json(&repo_config_path(root))?;
    let mut index = RepoIndex::default();
    for (name, spec) in &config.repositories {
        let local_metadata = Path::new(&spec.location).join("repository.json");
        if spec.location.starts_with("https://")
            || spec.trusted_key.is_some()
            || local_metadata.exists()
        {
            let key = spec.trusted_key.as_deref().ok_or_else(|| {
                ApmError::Transaction(format!(
                    "repository {name} has signed metadata but no explicitly trusted key"
                ))
            })?;
            let (metadata_bytes, base) = repository_metadata(&spec.location)?;
            let minimum = *config.metadata_versions.get(name).unwrap_or(&0);
            let (metadata, digest) = repository::verify(&metadata_bytes, key, minimum)?;
            if metadata.version == minimum
                && config
                    .metadata_digests
                    .get(name)
                    .is_some_and(|old| old != &digest)
            {
                return Err(ApmError::InvalidPackage(format!(
                    "repository {name} changed metadata without increasing its version"
                )));
            }
            let mut target_names = BTreeSet::new();
            for target in &metadata.packages {
                if !target_names.insert(target.file.clone()) {
                    return Err(ApmError::InvalidPackage(format!(
                        "duplicate repository target {}",
                        target.file
                    )));
                }
                let archive = fetch_target(&base, &target.file)?;
                if archive.len() as u64 != target.size
                    || apm::package::sha256(&archive) != target.sha256
                {
                    return Err(ApmError::InvalidPackage(format!(
                        "signed size/hash mismatch for {}",
                        target.file
                    )));
                }
                let cached_path = write_cached_package(root, name, &target.file, &archive)?;
                let verified = verify_package(&cached_path)?;
                if verified.manifest.package.name != target.name
                    || verified.manifest.package.version != target.version
                    || verified.manifest.package.release != target.release
                {
                    return Err(ApmError::InvalidPackage(format!(
                        "target metadata does not match archive {}",
                        target.file
                    )));
                }
                index.packages.push(CachedPackage {
                    repository: name.clone(),
                    path: cached_path,
                    manifest: verified.manifest,
                    expected_sha256: Some(target.sha256.clone()),
                });
            }
            config
                .metadata_versions
                .insert(name.clone(), metadata.version);
            config.metadata_digests.insert(name.clone(), digest);
        } else {
            let directory = Path::new(&spec.location);
            if !directory.is_dir() {
                return Err(ApmError::Transaction(format!(
                    "repository {name} directory is missing: {}",
                    directory.display()
                )));
            }
            let mut paths = Vec::new();
            for entry in fs::read_dir(directory)? {
                let entry = entry?;
                if entry.file_type()?.is_file()
                    && entry.path().extension().is_some_and(|ext| ext == "apkg")
                {
                    paths.push(entry.path());
                }
            }
            paths.sort();
            for path in paths {
                let verified = verify_package(&path).map_err(|e| {
                    ApmError::InvalidPackage(format!("repository {name}, {}: {e}", path.display()))
                })?;
                index.packages.push(CachedPackage {
                    repository: name.clone(),
                    path: path.canonicalize()?,
                    manifest: verified.manifest,
                    expected_sha256: None,
                });
            }
        }
    }
    index.packages.sort_by(|a, b| {
        a.manifest
            .package
            .name
            .cmp(&b.manifest.package.name)
            .then(a.manifest.package.version.cmp(&b.manifest.package.version))
            .then(a.manifest.package.release.cmp(&b.manifest.package.release))
    });
    for pair in index.packages.windows(2) {
        let a = &pair[0].manifest.package;
        let b = &pair[1].manifest.package;
        if a.name == b.name && a.version == b.version && a.release == b.release {
            return Err(ApmError::Transaction(format!(
                "duplicate repository package identity {} {}-{}",
                a.name, a.version, a.release
            )));
        }
    }
    save_state_json(root, "etc/apm/repositories.json", &config)?;
    save_state_json(root, "var/cache/apm/repositories.json", &index)?;
    println!(
        "updated local repository index: {} package archives verified",
        index.packages.len()
    );
    Ok(())
}

fn repository_metadata(location: &str) -> Result<(Vec<u8>, String)> {
    if location.starts_with("https://") {
        let mut base = reqwest::Url::parse(location)
            .map_err(|e| ApmError::Transaction(format!("invalid repository URL: {e}")))?;
        if base.scheme() != "https"
            || !base.username().is_empty()
            || base.password().is_some()
            || base.query().is_some()
            || base.fragment().is_some()
        {
            return Err(ApmError::Transaction(
                "repository URL must be HTTPS without credentials, query, or fragment".into(),
            ));
        }
        if !base.path().ends_with('/') {
            base.set_path(&format!("{}/", base.path()));
        }
        let target = base
            .join("repository.json")
            .map_err(|e| ApmError::Transaction(e.to_string()))?;
        let response = http_client()?
            .get(target)
            .send()
            .and_then(reqwest::blocking::Response::error_for_status)
            .map_err(|e| {
                ApmError::Transaction(format!("repository metadata download failed: {e}"))
            })?;
        if response.url().scheme() != "https" {
            return Err(ApmError::Transaction(
                "HTTPS repository redirected to a non-HTTPS URL".into(),
            ));
        }
        if response
            .content_length()
            .is_some_and(|length| length > 4 * 1024 * 1024)
        {
            return Err(ApmError::Transaction(
                "repository metadata exceeds 4 MiB".into(),
            ));
        }
        let bytes = response
            .bytes()
            .map_err(|e| ApmError::Transaction(e.to_string()))?
            .to_vec();
        if bytes.len() > 4 * 1024 * 1024 {
            return Err(ApmError::Transaction(
                "repository metadata exceeds 4 MiB".into(),
            ));
        }
        Ok((bytes, base.to_string()))
    } else {
        let directory = Path::new(location).canonicalize()?;
        Ok((
            fs::read(directory.join("repository.json"))?,
            directory.to_string_lossy().to_string(),
        ))
    }
}

fn fetch_target(base: &str, name: &str) -> Result<Vec<u8>> {
    if base.starts_with("https://") {
        let base = reqwest::Url::parse(base).map_err(|e| ApmError::Transaction(e.to_string()))?;
        let url = base
            .join(name)
            .map_err(|e| ApmError::Transaction(e.to_string()))?;
        let response = http_client()?
            .get(url)
            .send()
            .and_then(reqwest::blocking::Response::error_for_status)
            .map_err(|e| ApmError::Transaction(format!("package download failed: {e}")))?;
        if response.url().scheme() != "https" {
            return Err(ApmError::Transaction(
                "package download redirected to a non-HTTPS URL".into(),
            ));
        }
        if response
            .content_length()
            .is_some_and(|length| length > apm::package::MAX_PACKAGE_BYTES)
        {
            return Err(ApmError::InvalidPackage(
                "download exceeds package size limit".into(),
            ));
        }
        let bytes = response
            .bytes()
            .map_err(|e| ApmError::Transaction(e.to_string()))?
            .to_vec();
        if bytes.len() as u64 > apm::package::MAX_PACKAGE_BYTES {
            return Err(ApmError::InvalidPackage(
                "download exceeds package size limit".into(),
            ));
        }
        Ok(bytes)
    } else {
        let directory = Path::new(base).canonicalize()?;
        let source = directory.join(name);
        let canonical = source.canonicalize()?;
        if !canonical.starts_with(&directory) || !fs::symlink_metadata(&source)?.is_file() {
            return Err(ApmError::InvalidPackage(
                "repository target escapes repository directory".into(),
            ));
        }
        let bytes = fs::read(canonical)?;
        if bytes.len() as u64 > apm::package::MAX_PACKAGE_BYTES {
            return Err(ApmError::InvalidPackage(
                "package exceeds size limit".into(),
            ));
        }
        Ok(bytes)
    }
}

fn http_client() -> Result<reqwest::blocking::Client> {
    reqwest::blocking::Client::builder()
        .timeout(std::time::Duration::from_secs(30))
        .build()
        .map_err(|e| ApmError::Transaction(format!("cannot initialize HTTPS client: {e}")))
}

fn confirm(yes: bool) -> Result<bool> {
    if yes {
        return Ok(true);
    }
    print!("Proceed? [y/N] ");
    io::stdout().flush()?;
    let mut answer = String::new();
    io::stdin().read_line(&mut answer)?;
    Ok(matches!(answer.trim(), "y" | "Y" | "yes" | "YES"))
}
fn candidates(index: &RepoIndex) -> Vec<Manifest> {
    index.packages.iter().map(|p| p.manifest.clone()).collect()
}

fn is_newer(candidate: &Manifest, current: &Manifest) -> bool {
    match (
        semver::Version::parse(&candidate.package.version),
        semver::Version::parse(&current.package.version),
    ) {
        (Ok(candidate_version), Ok(current_version)) => {
            candidate_version > current_version
                || (candidate_version == current_version
                    && candidate.package.release > current.package.release)
        }
        _ => false,
    }
}

fn verify_cached(cached: &CachedPackage) -> Result<apm::package::VerifiedPackage> {
    let bytes = fs::read(&cached.path)?;
    if cached
        .expected_sha256
        .as_ref()
        .is_some_and(|expected| apm::package::sha256(&bytes) != *expected)
    {
        return Err(ApmError::InvalidPackage(format!(
            "repository package changed after index verification: {}",
            cached.path.display()
        )));
    }
    let package = verify_package(&cached.path)?;
    if package.manifest != cached.manifest {
        return Err(ApmError::InvalidPackage(format!(
            "package manifest changed after repository update: {}",
            cached.path.display()
        )));
    }
    Ok(package)
}
fn print_plan(title: &str, manifests: &[Manifest]) {
    println!("{title}:");
    if manifests.is_empty() {
        println!("  (no changes)");
    }
    for m in manifests {
        println!(
            "  + {} {}-{} [{} / {}]",
            m.package.name, m.package.version, m.package.release, m.package.arch, m.package.abi
        );
    }
}

fn install(root: &Path, names_or_paths: &[String], yes: bool) -> Result<()> {
    if names_or_paths.is_empty() {
        return Err(ApmError::Transaction(
            "install requires a package name or .apkg path".into(),
        ));
    }
    let index = load_index(root)?;
    let mut candidate_manifests = candidates(&index);
    let mut requested = Vec::new();
    for argument in names_or_paths {
        let path = PathBuf::from(argument);
        if path.extension().is_some_and(|ext| ext == "apkg") || path.exists() {
            let package = verify_package(&path)?;
            let name = package.manifest.package.name.clone();
            candidate_manifests.retain(|m| m.package.name != name);
            candidate_manifests.push(package.manifest);
            requested.push(name);
        } else {
            requested.push(argument.clone());
        }
    }
    let db = load_db(root)?;
    let installed = InstalledSet {
        versions: installer::installed_versions(&db),
    };
    let plan = resolver::resolve(&requested, &candidate_manifests, &installed)?;
    let install_list: Vec<_> = plan
        .install
        .into_iter()
        .filter(|m| {
            db.packages
                .get(&m.package.name)
                .is_none_or(|current| is_newer(m, &current.manifest))
        })
        .collect();
    print_plan("Install transaction", &install_list);
    if install_list.is_empty() {
        println!("all requested packages are already current");
        return Ok(());
    }
    if !confirm(yes)? {
        println!("cancelled");
        return Ok(());
    }
    let mut verified = Vec::new();
    for manifest in &install_list {
        let cached = index.packages.iter().find(|p| {
            p.manifest.package.name == manifest.package.name
                && p.manifest.package.version == manifest.package.version
                && p.manifest.package.release == manifest.package.release
        });
        let package = if let Some(cached) = cached {
            verify_cached(cached)?
        } else {
            let path = names_or_paths.iter().map(PathBuf::from).find(|path| path.extension().is_some_and(|ext| ext == "apkg") && verify_package(path).is_ok_and(|p| p.manifest.package.name == manifest.package.name))
                .ok_or_else(|| ApmError::Transaction(format!("archive source for {} is not in a configured local repository or explicit request", manifest.package.name)))?
            ;
            let package = verify_package(&path)?;
            if &package.manifest != manifest {
                return Err(ApmError::InvalidPackage(
                    "explicit package changed during transaction planning".into(),
                ));
            }
            package
        };
        verified.push(package);
    }
    let tx = installer::install(
        root,
        &db_path(root),
        &verified,
        "local repository or package file",
    )?;
    println!("transaction {tx} committed successfully");
    Ok(())
}

fn remove(root: &Path, names: &[String], yes: bool) -> Result<()> {
    if names.is_empty() {
        return Err(ApmError::Transaction(
            "remove requires at least one package name".into(),
        ));
    }
    let db = load_db(root)?;
    let selected: BTreeSet<_> = names.iter().cloned().collect();
    for (name, package) in &db.packages {
        if selected.contains(name) {
            continue;
        }
        for dependency in package.manifest.dependencies.keys() {
            if selected.contains(dependency) {
                return Err(ApmError::Transaction(format!(
                    "cannot remove {dependency}; installed package {name} depends on it"
                )));
            }
        }
    }
    let mut plan = Vec::new();
    for name in names {
        let p = db
            .packages
            .get(name)
            .ok_or_else(|| ApmError::Transaction(format!("package {name} is not installed")))?;
        plan.push(p.manifest.clone());
    }
    print_plan("Remove transaction", &plan);
    if !confirm(yes)? {
        println!("cancelled");
        return Ok(());
    }
    let tx = installer::remove_many(root, &db_path(root), names)?;
    println!("removed {} package(s) (transaction {tx})", selected.len());
    Ok(())
}

fn run(cli: Cli) -> Result<()> {
    let root = prepare_root(&cli.root)?;
    match cli.command {
        Command::Update => update(&root),
        Command::Search { query } => {
            let index = load_index(&root)?;
            for p in index.packages.iter().filter(|p| {
                p.manifest.package.name.contains(&query)
                    || p.manifest
                        .package
                        .description
                        .to_lowercase()
                        .contains(&query.to_lowercase())
            }) {
                println!(
                    "{} {}-{}  {}  ({})",
                    p.manifest.package.name,
                    p.manifest.package.version,
                    p.manifest.package.release,
                    p.manifest.package.description,
                    p.repository
                );
            }
            Ok(())
        }
        Command::Info { package } => {
            let index = load_index(&root)?;
            let p = index
                .packages
                .iter()
                .find(|p| p.manifest.package.name == package)
                .ok_or_else(|| {
                    ApmError::Transaction(format!("package {package} not found; run `apm update`"))
                })?;
            println!(
                "{} {}-{}\nArchitecture: {}\nABI: {}\nLicense: {}\nRepository: {}\n{}",
                p.manifest.package.name,
                p.manifest.package.version,
                p.manifest.package.release,
                p.manifest.package.arch,
                p.manifest.package.abi,
                p.manifest.package.license,
                p.repository,
                p.manifest.package.description
            );
            for (name, req) in &p.manifest.dependencies {
                println!("Depends: {name} {req}");
            }
            Ok(())
        }
        Command::Install { packages } => install(&root, &packages, cli.yes),
        Command::Remove { packages } => remove(&root, &packages, cli.yes),
        Command::List => {
            let db = load_db(&root)?;
            for (name, p) in db.packages {
                println!(
                    "{name} {}-{} [{}]",
                    p.manifest.package.version, p.manifest.package.release, p.manifest.package.abi
                );
            }
            Ok(())
        }
        Command::Upgrade { package } => {
            let db = load_db(&root)?;
            if let Some(name) = package.as_deref() {
                if !db.packages.contains_key(name) {
                    return Err(ApmError::Transaction(format!(
                        "package {name} is not installed"
                    )));
                }
            }
            let names: Vec<String> = package
                .map(|p| vec![p])
                .unwrap_or_else(|| db.packages.keys().cloned().collect());
            let index = load_index(&root)?;
            let mut plan = resolver::resolve(
                &names,
                &candidates(&index),
                &InstalledSet {
                    versions: installer::installed_versions(&db),
                },
            )?
            .install;
            plan.retain(|candidate| {
                db.packages
                    .get(&candidate.package.name)
                    .is_some_and(|old| is_newer(candidate, &old.manifest))
            });
            print_plan("Upgrade transaction", &plan);
            if plan.is_empty() {
                println!("all selected packages are current");
                return Ok(());
            }
            if !confirm(cli.yes)? {
                println!("cancelled");
                return Ok(());
            }
            let mut verified = Vec::new();
            for manifest in &plan {
                let cached = index
                    .packages
                    .iter()
                    .find(|p| {
                        p.manifest.package.name == manifest.package.name
                            && p.manifest.package.version == manifest.package.version
                            && p.manifest.package.release == manifest.package.release
                    })
                    .ok_or_else(|| {
                        ApmError::Transaction("missing repository package archive".into())
                    })?;
                verified.push(verify_cached(cached)?);
            }
            let tx = installer::install(
                &root,
                &db_path(&root),
                &verified,
                "local repository upgrade",
            )?;
            println!("transaction {tx} committed successfully");
            Ok(())
        }
        Command::Verify { package } => {
            let path = PathBuf::from(&package);
            if path.extension().is_some_and(|ext| ext == "apkg") {
                let p = verify_package(&path)?;
                println!(
                    "archive verified: {} {}-{}",
                    p.manifest.package.name, p.manifest.package.version, p.manifest.package.release
                );
            } else {
                let db = load_db(&root)?;
                let installed: &InstalledPackage = db.packages.get(&package).ok_or_else(|| {
                    ApmError::Transaction(format!("package {package} is not installed"))
                })?;
                let one = Database {
                    schema_version: 1,
                    packages: [(package.clone(), installed.clone())].into_iter().collect(),
                    history: Vec::new(),
                };
                let issues = installer::check_database_files(&root, &one)?;
                if !issues.is_empty() {
                    return Err(ApmError::Transaction(format!(
                        "verification failed:\n{}",
                        issues.join("\n")
                    )));
                }
                println!("installed package verified: {package}");
            }
            Ok(())
        }
        Command::Clean => {
            let path = root.join("var/cache/apm");
            if let Ok(meta) = fs::symlink_metadata(&path) {
                if meta.file_type().is_symlink() || !meta.is_dir() {
                    return Err(ApmError::Transaction(
                        "refusing unsafe package cache path".into(),
                    ));
                }
                fs::remove_dir_all(&path)?;
            }
            println!("package cache cleaned");
            Ok(())
        }
        Command::History => {
            let db = load_db(&root)?;
            for e in db.history {
                println!(
                    "{} {} {} {} tx={}",
                    e.timestamp, e.action, e.package, e.version, e.transaction
                );
            }
            Ok(())
        }
        Command::Recover => {
            let count = installer::recover(&root, &db_path(&root))?;
            println!("recovered or cleaned {count} incomplete transaction(s)");
            Ok(())
        }
        Command::Repo { command } => {
            let config_path = repo_config_path(&root);
            let mut config: RepoConfig = load_json(&config_path)?;
            match command {
                RepoCommand::List => {
                    for (name, spec) in config.repositories {
                        println!(
                            "{name} {}{}",
                            spec.location,
                            if spec.trusted_key.is_some() {
                                " [pinned key]"
                            } else {
                                " [unsigned local]"
                            }
                        );
                    }
                    Ok(())
                }
                RepoCommand::Add {
                    name,
                    location,
                    key,
                } => {
                    if name.is_empty()
                        || !name
                            .bytes()
                            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
                    {
                        return Err(ApmError::Transaction(
                            "repository name must contain only letters, digits, '_' or '-'".into(),
                        ));
                    }
                    let normalized = if location.starts_with("https://") {
                        if key.is_none() {
                            return Err(ApmError::Transaction(
                                "HTTPS repositories require an explicitly trusted Ed25519 --key"
                                    .into(),
                            ));
                        }
                        let parsed = reqwest::Url::parse(&location).map_err(|e| {
                            ApmError::Transaction(format!("invalid repository URL: {e}"))
                        })?;
                        if parsed.scheme() != "https"
                            || !parsed.username().is_empty()
                            || parsed.password().is_some()
                            || parsed.query().is_some()
                            || parsed.fragment().is_some()
                        {
                            return Err(ApmError::Transaction("repository URL must be HTTPS without credentials, query, or fragment".into()));
                        }
                        location
                    } else {
                        let canonical = Path::new(&location).canonicalize().map_err(|e| {
                            ApmError::Transaction(format!("repository path is unavailable: {e}"))
                        })?;
                        if !canonical.is_dir() {
                            return Err(ApmError::Transaction(
                                "local repository must be a directory".into(),
                            ));
                        }
                        canonical.to_string_lossy().to_string()
                    };
                    if let Some(key) = &key {
                        repository::parse_public_key(key)?;
                    }
                    if config.repositories.contains_key(&name) {
                        return Err(ApmError::Transaction(format!(
                            "repository {name} already exists"
                        )));
                    }
                    config.repositories.insert(
                        name.clone(),
                        RepoSpec {
                            location: normalized,
                            trusted_key: key,
                        },
                    );
                    save_state_json(&root, "etc/apm/repositories.json", &config)?;
                    println!("added repository {name}");
                    Ok(())
                }
                RepoCommand::Remove { name } => {
                    if config.repositories.remove(&name).is_none() {
                        return Err(ApmError::Transaction(format!(
                            "repository {name} does not exist"
                        )));
                    }
                    config.metadata_versions.remove(&name);
                    config.metadata_digests.remove(&name);
                    save_state_json(&root, "etc/apm/repositories.json", &config)?;
                    let mut index: RepoIndex = load_index(&root)?;
                    index.packages.retain(|p| p.repository != name);
                    save_state_json(&root, "var/cache/apm/repositories.json", &index)?;
                    println!("removed repository {name} and its cached package index entries");
                    Ok(())
                }
            }
        }
    }
}

fn main() {
    if let Err(error) = run(Cli::parse()) {
        eprintln!("apm: {error}");
        std::process::exit(1);
    }
}
