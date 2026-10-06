//! The shipped profiles are valid; every negative vector is rejected, for the stated
//! reason; the committed header is what the generator emits; selection fails loudly.

use std::path::{Path, PathBuf};

use enginegen::{emit, profile};

fn engines_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engines")
}

#[test]
fn shipped_profiles_are_valid() {
    let all = profile::load_dir(&engines_dir()).expect("engines/ must validate");
    let ids: Vec<_> = all.iter().map(|c| c.profile.engine_id.as_str()).collect();
    assert_eq!(ids, ["bmw-b58", "bmw-n20"]);
}

#[test]
fn every_negative_vector_is_rejected_for_its_reason() {
    let dir = engines_dir().join("vectors/invalid");
    let mut n = 0;
    for e in std::fs::read_dir(&dir).unwrap() {
        let path = e.unwrap().path();
        if path.extension().and_then(|s| s.to_str()) != Some("yaml") {
            continue;
        }
        let text = std::fs::read_to_string(&path).unwrap();
        let expect = text
            .lines()
            .find_map(|l| l.strip_prefix("# expect-error:"))
            .unwrap_or_else(|| panic!("{}: no `# expect-error:` header", path.display()))
            .trim()
            .to_string();
        match profile::load(&path) {
            Ok(_) => panic!("{} was accepted but must be rejected ({expect})", path.display()),
            Err(e) => assert!(
                e.contains(&expect),
                "{}: rejected with\n{e}\nbut expected it to mention `{expect}`",
                path.display()
            ),
        }
        n += 1;
    }
    assert!(n >= 20, "only {n} negative vectors");
}

#[test]
fn committed_header_is_current() {
    let all = profile::load_dir(&engines_dir()).unwrap();
    let sel = profile::select(&all, "all").unwrap();
    let want = emit::header(&all, &sel);
    let have = std::fs::read_to_string(engines_dir().join("../lib/cairn_engine/gen/cairn_engines_gen.h")).unwrap();
    assert_eq!(have, want, "run `make engines-gen` and commit the result");
}

#[test]
fn selection_is_strict() {
    let all = profile::load_dir(&engines_dir()).unwrap();
    assert!(profile::select(&all, "all").unwrap().len() == 2);
    assert_eq!(profile::select(&all, "bmw-n20").unwrap().len(), 1);
    assert_eq!(profile::select(&all, "bmw-n20, bmw-b58").unwrap().len(), 2);
    for bad in ["", "  ", "bmw-n21", "bmw-n20,nope", "bmw-n20,bmw-n20", "all,bmw-n20", "bmw-n20,,bmw-b58"] {
        assert!(profile::select(&all, bad).is_err(), "`{bad}` must be refused");
    }
}

#[test]
fn identity_depends_on_content_and_selection() {
    let all = profile::load_dir(&engines_dir()).unwrap();
    let one = emit::identity(&profile::select(&all, "bmw-n20").unwrap());
    let both = emit::identity(&profile::select(&all, "all").unwrap());
    assert_ne!(one.build_sha256, both.build_sha256);
    assert!(one.string.contains("bmw-n20@1/") && !one.string.contains("bmw-b58"));

    // The same selection in any order is the same build.
    let rev = emit::identity(&profile::select(&all, "bmw-n20,bmw-b58").unwrap());
    assert_eq!(rev.build_sha256, both.build_sha256);

    // One changed byte in a profile changes its hash, and so the build's.
    let path = engines_dir().join("bmw-n20.yaml");
    let mut bytes = std::fs::read(&path).unwrap();
    assert_eq!(profile::content_hash(&bytes), all.iter().find(|c| c.profile.engine_id == "bmw-n20").unwrap().sha256);
    bytes.extend(b"# edited\n");
    assert_ne!(profile::content_hash(&bytes), all.iter().find(|c| c.profile.engine_id == "bmw-n20").unwrap().sha256);

    // CRLF checkouts hash the same as LF ones.
    let lf = b"a: 1\nb: 2\n";
    let crlf = b"a: 1\r\nb: 2\r\n";
    assert_eq!(profile::content_hash(lf), profile::content_hash(crlf));
}

#[test]
fn a_broken_profile_fails_the_whole_directory() {
    let tmp = std::env::temp_dir().join(format!("enginegen-test-{}", std::process::id()));
    std::fs::create_dir_all(&tmp).unwrap();
    std::fs::copy(engines_dir().join("bmw-n20.yaml"), tmp.join("bmw-n20.yaml")).unwrap();
    std::fs::copy(
        engines_dir().join("vectors/invalid/bad-div-by-zero.yaml"),
        tmp.join("bad-div-by-zero.yaml"),
    )
    .unwrap();
    let err = profile::load_dir(&tmp).unwrap_err();
    assert!(err.contains("bad-div-by-zero.yaml") && err.contains("divisor can be zero"), "{err}");
    std::fs::remove_dir_all(&tmp).ok();
}

fn as_json(path: &Path) -> serde_json::Value {
    let y: serde_yaml::Value = serde_yaml::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    serde_json::to_value(&y).unwrap()
}

/// The draft JSON Schema, which other consumers will use, agrees with the generator on
/// the shipped profiles and on every structural negative. (Semantic negatives, such as a
/// formula that can divide by zero, are beyond JSON Schema and are the generator's.)
#[test]
fn draft_schema_agrees_with_the_generator() {
    let schema = serde_json::from_str::<serde_json::Value>(
        &std::fs::read_to_string(engines_dir().join("engine.schema.draft.json")).unwrap(),
    )
    .unwrap();
    let compiled = jsonschema::JSONSchema::compile(&schema).expect("the schema itself is valid");

    for id in ["bmw-n20", "bmw-b58"] {
        let doc = as_json(&engines_dir().join(format!("{id}.yaml")));
        let res = compiled.validate(&doc);
        if let Err(errs) = res {
            let msgs: Vec<String> = errs.map(|e| format!("{} at {}", e, e.instance_path)).collect();
            panic!("{id} does not satisfy the draft schema: {msgs:?}");
        }
    }

    for name in ["bad-unknown-field", "bad-missing-section", "bad-schema-version", "bad-vin-pattern", "bad-cold-no-slot"] {
        let doc = as_json(&engines_dir().join(format!("vectors/invalid/{name}.yaml")));
        assert!(!compiled.is_valid(&doc), "{name} must fail the draft schema as well");
    }
}
