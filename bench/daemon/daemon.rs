// The daemon shape — Rust reference. A faithful translation of daemon.goo:
// same splits, same map, same pushes, same join.
//
// This is deliberately the IDIOMATIC version, not a zero-allocation rewrite.
// It allocates a Vec, a HashMap and several Strings per request exactly as the
// Goo and Go versions do. The only difference is that Rust frees them when
// they go out of scope, which is the property under measurement.
use std::collections::HashMap;
use std::env;

fn handle(req: &str) -> String {
    let fields: Vec<&str> = req.split(',').collect();
    let mut counts: HashMap<String, i64> = HashMap::new();
    let mut total: i64 = 0;
    for f in fields.iter() {
        let f = f.trim();
        match f.parse::<i64>() {
            Ok(n) => {
                total += n;
                *counts.entry(f.to_string()).or_insert(0) += 1;
            }
            Err(_) => {
                counts.insert(f.to_uppercase(), 1);
            }
        }
    }
    let mut parts: Vec<String> = Vec::new();
    for f in fields.iter() {
        parts.push(f.trim().to_string());
    }
    format!("{}:{}:{}", total, counts.len(), parts.join("|"))
}

fn main() {
    let mut requests: i64 = 50000;
    if let Some(a) = env::args().nth(1) {
        if let Ok(n) = a.parse::<i64>() {
            requests = n;
        }
    }
    let mut last = String::new();
    for _ in 0..requests {
        last = handle("1, 2, 3, four, 5, six, 7");
    }
    println!("{}", last.len());
}
