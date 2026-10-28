use crate::{
    package::{validate_manifest, Manifest},
    ApmError, Result,
};
use semver::{Version, VersionReq};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Debug, Default)]
pub struct InstalledSet {
    pub versions: BTreeMap<String, String>,
}

#[derive(Clone, Debug)]
pub struct Plan {
    pub install: Vec<Manifest>,
    pub remove: Vec<String>,
}

pub fn resolve(
    requests: &[String],
    candidates: &[Manifest],
    installed: &InstalledSet,
) -> Result<Plan> {
    let mut resolver = Resolver {
        candidates,
        installed,
        selected: BTreeMap::new(),
        ordered: Vec::new(),
        visiting: Vec::new(),
        done: BTreeSet::new(),
    };
    for request in requests {
        resolver.visit(request, None)?;
    }
    for candidate in resolver.selected.values() {
        for conflict in &candidate.conflicts {
            if resolver.selected.contains_key(conflict)
                || (installed.versions.contains_key(conflict)
                    && !resolver.selected.contains_key(conflict))
            {
                return Err(ApmError::Resolution(format!(
                    "{} conflicts with installed/selected package {conflict}",
                    candidate.package.name
                )));
            }
        }
    }
    Ok(Plan {
        install: resolver.ordered,
        remove: Vec::new(),
    })
}

struct Resolver<'a> {
    candidates: &'a [Manifest],
    installed: &'a InstalledSet,
    selected: BTreeMap<String, Manifest>,
    ordered: Vec<Manifest>,
    visiting: Vec<String>,
    done: BTreeSet<String>,
}

impl Resolver<'_> {
    fn visit(&mut self, name: &str, requirement: Option<&str>) -> Result<()> {
        if self.done.contains(name) {
            return Ok(());
        }
        if let Some(start) = self.visiting.iter().position(|item| item == name) {
            let mut cycle = self.visiting[start..].join(" -> ");
            cycle.push_str(" -> ");
            cycle.push_str(name);
            return Err(ApmError::Resolution(format!("dependency cycle: {cycle}")));
        }
        if let (Some(req), Some(version)) = (requirement, self.installed.versions.get(name)) {
            let req = VersionReq::parse(req).map_err(|e| {
                ApmError::Resolution(format!("invalid requirement for {name}: {e}"))
            })?;
            if Version::parse(version).is_ok_and(|v| req.matches(&v)) {
                self.done.insert(name.to_string());
                return Ok(());
            }
        }
        let manifest = choose(name, requirement, self.candidates, self.installed)?;
        let package_name = manifest.package.name.clone();
        if self.selected.contains_key(&package_name) {
            self.done.insert(name.to_string());
            return Ok(());
        }
        self.visiting.push(name.to_string());
        for (dependency, requirement) in &manifest.dependencies {
            self.visit(dependency, Some(requirement))?;
        }
        self.visiting.pop();
        self.done.insert(name.to_string());
        self.done.insert(package_name.clone());
        self.selected.insert(package_name, manifest.clone());
        self.ordered.push(manifest);
        Ok(())
    }
}

fn choose(
    name: &str,
    requirement: Option<&str>,
    candidates: &[Manifest],
    installed: &InstalledSet,
) -> Result<Manifest> {
    let requirement = requirement
        .map(VersionReq::parse)
        .transpose()
        .map_err(|e| ApmError::Resolution(format!("invalid requirement for {name}: {e}")))?;
    let mut available: Vec<&Manifest> = candidates
        .iter()
        .filter(|m| {
            validate_manifest(m, true).is_ok()
                && (m.package.name == name || m.provides.iter().any(|provided| provided == name))
                && requirement.as_ref().is_none_or(|req| {
                    Version::parse(&m.package.version).is_ok_and(|version| req.matches(&version))
                })
        })
        .collect();
    let _ = installed;
    available.sort_by(|a, b| {
        Version::parse(&a.package.version)
            .unwrap()
            .cmp(&Version::parse(&b.package.version).unwrap())
            .then(a.package.release.cmp(&b.package.release))
    });
    available.pop().cloned().ok_or_else(|| {
        ApmError::Resolution(format!(
            "no candidate for {name} satisfies {}",
            requirement.map_or_else(|| "any version".into(), |r| r.to_string())
        ))
    })
}
