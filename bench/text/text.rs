// String and slice heavy text processing — Rust reference. See text.goo.
//
// Idiomatic, not a zero-allocation rewrite: it builds a String per line, a Vec
// of word slices, a Vec of owned uppercase Strings, and a joined String, the
// same shapes the Goo and Go versions build. Rust frees them at scope exit.
use std::env;

fn main() {
    let mut lines: i64 = 200000;
    if let Some(a) = env::args().nth(1) {
        if let Ok(n) = a.parse::<i64>() {
            lines = n;
        }
    }
    let mut hits: i64 = 0;
    let mut total: i64 = 0;
    for i in 0..lines {
        let line = format!("alpha beta gamma {} delta epsilon zeta", i);
        let words: Vec<&str> = line.split_whitespace().collect();
        let mut kept: Vec<String> = Vec::new();
        for w in words.iter() {
            if w.find('a').is_some() {
                hits += 1;
                kept.push(w.to_uppercase());
            }
            total += w.len() as i64;
        }
        let joined = kept.join(",");
        total += joined.matches(',').count() as i64;
    }
    println!("{}", total + hits);
}
