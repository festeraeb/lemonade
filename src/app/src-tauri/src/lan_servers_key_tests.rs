//! N2b: a saved choice carries the server's device key. Run: cargo test --lib lan_servers
use super::*;

const KEY: &str = "ed25519:f6Y3aCO2va1Ht3WvqmkTo6J_VBcAoWrV5nEM8SOJ4vY";

#[test]
fn an_accepted_choice_records_the_key() {
    let mut saved = BTreeMap::new();
    apply_choice_with_key(&mut saved, "desk:13305", "accepted", "desk", "http://192.168.4.10:13305", Some(KEY)).unwrap();
    let c = &saved["desk:13305"];
    assert_eq!(c.choice, Choice::Accepted);
    assert_eq!((c.hostname.as_str(), c.url.as_str(), c.public_key.as_str()), ("desk", "http://192.168.4.10:13305", KEY));
}

#[test]
fn a_later_choice_without_a_key_keeps_the_saved_one() {
    let mut saved = BTreeMap::new();
    apply_choice_with_key(&mut saved, "desk:13305", "accepted", "desk", "http://a:13305", Some(KEY)).unwrap();
    apply_choice_with_key(&mut saved, "desk:13305", "ignored", "desk", "http://b:13305", None).unwrap();
    assert_eq!(saved["desk:13305"].choice, Choice::Ignored);
    assert_eq!(saved["desk:13305"].url, "http://b:13305");
    assert_eq!(saved["desk:13305"].public_key, KEY);
    apply_choice_with_key(&mut saved, "desk:13305", "accepted", "desk", "http://b:13305", Some("")).unwrap();
    assert_eq!(saved["desk:13305"].public_key, KEY, "an empty key counts as none");
    // the old entry point keeps it too
    apply_choice(&mut saved, "desk:13305", "ignored", "desk", "http://b:13305").unwrap();
    assert_eq!(saved["desk:13305"].public_key, KEY);
}

#[test]
fn a_new_key_replaces_the_old_and_new_forgets_it() {
    let mut saved = BTreeMap::new();
    apply_choice_with_key(&mut saved, "desk:13305", "accepted", "desk", "", Some(KEY)).unwrap();
    apply_choice_with_key(&mut saved, "desk:13305", "accepted", "desk", "", Some("ed25519:AAAA")).unwrap();
    assert_eq!(saved["desk:13305"].public_key, "ed25519:AAAA");
    apply_choice_with_key(&mut saved, "desk:13305", "new", "", "", None).unwrap();
    assert!(!saved.contains_key("desk:13305"));
    assert!(apply_choice_with_key(&mut saved, "x", "bogus", "", "", Some(KEY)).is_err());
}

#[test]
fn the_file_format_stays_compatible() {
    // an entry saved before keys were recorded still reads, with an empty key
    let old: SavedChoice = serde_json::from_str(r#"{"choice":"accepted","hostname":"desk","url":"http://a:13305","updated":5}"#).unwrap();
    assert_eq!(old.public_key, "");
    // an empty key is left out of the file; a key is written
    let mut saved = BTreeMap::new();
    apply_choice(&mut saved, "a", "accepted", "a", "").unwrap();
    assert!(!serde_json::to_string(&saved["a"]).unwrap().contains("public_key"));
    apply_choice_with_key(&mut saved, "b", "accepted", "b", "", Some(KEY)).unwrap();
    assert!(serde_json::to_string(&saved["b"]).unwrap().contains(KEY));
}
