use apm::{
    database::Database,
    installer,
    package::{
        build_package, sha256, validate_destination, Manifest, PackageFile, PackageMetadata,
        VerifiedPackage, FORMAT_VERSION,
    },
    resolver::{self, InstalledSet},
};
use std::{collections::BTreeMap, fs, path::Path};
use tempfile::tempdir;

fn manifest(name: &str, version: &str, dependencies: &[(&str, &str)]) -> Manifest {
    Manifest {
        format_version: FORMAT_VERSION,
        package: PackageMetadata {
            name: name.into(),
            version: version.into(),
            release: 1,
            arch: "x86_64".into(),
            abi: "anix-1".into(),
            description: format!("{name} test package"),
        license: "GPL-3.0-only".into(),
        },
        dependencies: dependencies
            .iter()
            .map(|(n, v)| ((*n).into(), (*v).into()))
            .collect(),
        optional_dependencies: BTreeMap::new(),
        conflicts: Vec::new(),
        provides: Vec::new(),
        files: vec![PackageFile {
            archive_path: "payload/00000000".into(),
            destination: format!("/usr/bin/{name}"),
            sha256: sha256(name.as_bytes()),
            mode: 0o755,
            source: None,
        }],
    }
}

fn minimal_anix_elf() -> Vec<u8> {
    let mut bytes = vec![0u8; 128];
    bytes[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
    bytes[16..18].copy_from_slice(&2u16.to_le_bytes());
    bytes[18..20].copy_from_slice(&62u16.to_le_bytes());
    bytes[20..24].copy_from_slice(&1u32.to_le_bytes());
    bytes[24..32].copy_from_slice(&0x400078u64.to_le_bytes());
    bytes[32..40].copy_from_slice(&64u64.to_le_bytes());
    bytes[52..54].copy_from_slice(&64u16.to_le_bytes());
    bytes[54..56].copy_from_slice(&56u16.to_le_bytes());
    bytes[56..58].copy_from_slice(&1u16.to_le_bytes());
    bytes[64..68].copy_from_slice(&1u32.to_le_bytes());
    bytes[68..72].copy_from_slice(&5u32.to_le_bytes());
    bytes[72..80].copy_from_slice(&0u64.to_le_bytes());
    bytes[80..88].copy_from_slice(&0x400000u64.to_le_bytes());
    bytes[96..104].copy_from_slice(&128u64.to_le_bytes());
    bytes[104..112].copy_from_slice(&128u64.to_le_bytes());
    bytes[112..120].copy_from_slice(&0x1000u64.to_le_bytes());
    bytes
}

fn write_definition(directory: &Path, source: &[u8]) -> std::path::PathBuf {
    fs::create_dir_all(directory.join("files")).unwrap();
    fs::write(directory.join("files/app"), source).unwrap();
    let definition = r#"
format_version = 1

[package]
name = "demo"
version = "1.2.3"
release = 1
arch = "x86_64"
abi = "anix-1"
description = "test application"
license = "GPL-3.0-only"

[[files]]
archive_path = ""
destination = "/usr/bin/demo"
sha256 = ""
mode = 493
source = "files/app"
"#;
    fs::write(directory.join("package.toml"), definition).unwrap();
    directory.join("package.toml")
}

#[test]
fn package_build_is_reproducible_and_round_trips() {
    let temp = tempdir().unwrap();
    let executable = minimal_anix_elf();
    let def = write_definition(temp.path(), &executable);
    let first = temp.path().join("first.apkg");
    let second = temp.path().join("second.apkg");
    let result = build_package(&def, &first).unwrap();
    build_package(&def, &second).unwrap();
    assert_eq!(fs::read(&first).unwrap(), fs::read(&second).unwrap());
    assert_eq!(result.files[0].sha256, sha256(&executable));
    let verified = apm::package::verify_package(&first).unwrap();
    assert_eq!(verified.manifest.package.name, "demo");
    assert_eq!(verified.payload["payload/00000000"], executable);
}

#[test]
fn archive_verifier_rejects_payload_hash_mismatches() {
    let temp = tempdir().unwrap();
    let executable = minimal_anix_elf();
    let definition = write_definition(temp.path(), &executable);
    let good = temp.path().join("good.apkg");
    build_package(&definition, &good).unwrap();
    let mut manifest = apm::package::verify_package(&good).unwrap().manifest;
    manifest.files[0].sha256 = sha256(&executable);
    let manifest_text = toml::to_string(&manifest).unwrap();
    let bad = temp.path().join("bad-hash.apkg");
    let encoder = zstd::Encoder::new(fs::File::create(&bad).unwrap(), 1).unwrap();
    let mut archive = tar::Builder::new(encoder);
    for (name, bytes) in [
        ("manifest.toml", manifest_text.as_bytes()),
        ("payload/00000000", b"tampered".as_slice()),
    ] {
        let mut header = tar::Header::new_gnu();
        header.set_size(bytes.len() as u64);
        header.set_mode(if name == "manifest.toml" {
            0o644
        } else {
            0o755
        });
        header.set_uid(0);
        header.set_gid(0);
        header.set_mtime(0);
        header.set_entry_type(tar::EntryType::Regular);
        header.set_cksum();
        archive.append_data(&mut header, name, bytes).unwrap();
    }
    archive.into_inner().unwrap().finish().unwrap();
    assert!(apm::package::verify_package(&bad)
        .unwrap_err()
        .to_string()
        .contains("hash mismatch"));
}

#[test]
fn rejects_unsafe_destinations_and_symlink_package_sources() {
    assert!(validate_destination("/usr/bin/../../etc/passwd").is_err());
    assert!(validate_destination("/etc/passwd").is_err());
    let temp = tempdir().unwrap();
    let def = write_definition(temp.path(), b"payload");
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink("/etc/passwd", temp.path().join("files/link")).unwrap();
        let mut text = fs::read_to_string(&def).unwrap();
        text = text.replace("files/app", "files/link");
        fs::write(&def, text).unwrap();
        assert!(build_package(&def, &temp.path().join("bad.apkg")).is_err());
    }
}

#[test]
fn resolver_orders_dependencies_and_reports_missing_cycles_and_conflicts() {
    let dep = manifest("libdemo", "1.4.0", &[]);
    let app = manifest("demo", "2.0.0", &[("libdemo", ">=1.2, <2")]);
    let plan = resolver::resolve(
        &["demo".into()],
        &[app.clone(), dep.clone()],
        &InstalledSet::default(),
    )
    .unwrap();
    assert_eq!(
        plan.install
            .iter()
            .map(|m| m.package.name.as_str())
            .collect::<Vec<_>>(),
        ["libdemo", "demo"]
    );
    assert!(resolver::resolve(
        &["missing".into()],
        std::slice::from_ref(&app),
        &InstalledSet::default()
    )
    .is_err());
    let cycle_a = manifest("a", "1.0.0", &[("b", "*")]);
    let cycle_b = manifest("b", "1.0.0", &[("a", "*")]);
    assert!(
        resolver::resolve(&["a".into()], &[cycle_a, cycle_b], &InstalledSet::default())
            .unwrap_err()
            .to_string()
            .contains("cycle")
    );
    let mut conflict = app;
    conflict.conflicts = vec!["libdemo".into()];
    assert!(
        resolver::resolve(&["demo".into()], &[conflict, dep], &InstalledSet::default())
            .unwrap_err()
            .to_string()
            .contains("conflicts")
    );
}

fn verified(name: &str, content: &[u8]) -> VerifiedPackage {
    let mut m = manifest(name, "1.0.0", &[]);
    m.files[0].sha256 = sha256(content);
    let payload = [(m.files[0].archive_path.clone(), content.to_vec())]
        .into_iter()
        .collect();
    VerifiedPackage {
        manifest: m,
        payload,
    }
}

#[test]
fn installer_commits_lists_verifies_and_removes_under_isolated_root() {
    let temp = tempdir().unwrap();
    let root = temp.path().join("target-root");
    let db = root.join("var/lib/apm/installed.json");
    let executable = minimal_anix_elf();
    installer::install(&root, &db, &[verified("demo", &executable)], "test").unwrap();
    assert_eq!(fs::read(root.join("usr/bin/demo")).unwrap(), executable);
    let state = Database::load(&db).unwrap();
    assert!(state.packages.contains_key("demo"));
    assert!(installer::check_database_files(&root, &state)
        .unwrap()
        .is_empty());
    installer::remove(&root, &db, "demo").unwrap();
    assert!(!root.join("usr/bin/demo").exists());
    assert!(!Database::load(&db).unwrap().packages.contains_key("demo"));
}

#[test]
fn installer_refuses_unowned_or_modified_files() {
    let temp = tempdir().unwrap();
    let root = temp.path().join("root");
    let db = root.join("var/lib/apm/installed.json");
    fs::create_dir_all(root.join("usr/bin")).unwrap();
    fs::write(root.join("usr/bin/demo"), b"user data").unwrap();
    let executable = minimal_anix_elf();
    assert!(installer::install(&root, &db, &[verified("demo", &executable)], "test").is_err());
    fs::remove_file(root.join("usr/bin/demo")).unwrap();
    installer::install(&root, &db, &[verified("demo", &executable)], "test").unwrap();
    fs::write(root.join("usr/bin/demo"), b"local edit").unwrap();
    assert!(installer::remove(&root, &db, "demo").is_err());
    assert_eq!(fs::read(root.join("usr/bin/demo")).unwrap(), b"local edit");
}

#[test]
fn installer_refuses_symlinked_destination_ancestors() {
    let temp = tempdir().unwrap();
    let root = temp.path().join("root");
    let outside = temp.path().join("outside");
    fs::create_dir_all(&root).unwrap();
    fs::create_dir_all(&outside).unwrap();
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(&outside, root.join("usr")).unwrap();
        let db = root.join("var/lib/apm/installed.json");
        assert!(
            installer::install(&root, &db, &[verified("demo", &minimal_anix_elf())], "test")
                .is_err()
        );
        assert!(!outside.join("bin/demo").exists());
    }
}

#[test]
fn recover_rolls_back_an_interrupted_install() {
    let temp = tempdir().unwrap();
    let root = temp.path().join("root");
    let db = root.join("var/lib/apm/installed.json");
    let tx = db.parent().unwrap().join("transactions/fixture");
    fs::create_dir_all(tx.join("backup")).unwrap();
    fs::create_dir_all(root.join("usr/bin")).unwrap();
    fs::write(root.join("usr/bin/demo"), b"new interrupted file").unwrap();
    fs::write(tx.join("backup/0"), b"old file").unwrap();
    let journal = serde_json::json!({"version":1,"state":"committing","database_existed":false,
        "files":[{"destination":"/usr/bin/demo","had_original":true,"new_index":0}]});
    fs::write(
        tx.join("journal.json"),
        serde_json::to_vec(&journal).unwrap(),
    )
    .unwrap();
    assert_eq!(installer::recover(&root, &db).unwrap(), 1);
    assert_eq!(fs::read(root.join("usr/bin/demo")).unwrap(), b"old file");
}

#[test]
fn manifest_schema_rejects_invalid_version_and_non_anix_targets() {
    let mut bad = manifest("demo", "not-a-version", &[]);
    assert!(apm::package::validate_manifest(&bad, true).is_err());
    bad.package.version = "1.0.0".into();
    bad.package.abi = "linux".into();
    assert!(apm::package::validate_manifest(&bad, true).is_err());
}
