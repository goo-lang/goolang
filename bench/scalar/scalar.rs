// Scalar loop, no allocation — Rust reference. See scalar.goo for the
// rationale. Go and Goo wrap on signed overflow, so wrapping_mul and
// wrapping_add here are the same machine operation, not a slower one.
fn main() {
    let mut x: i64 = 1;
    let mut acc: i64 = 0;
    let mut i: i64 = 0;
    while i < 1000000000 {
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        acc = acc.wrapping_add(x >> 33);
        i += 1;
    }
    println!("{}", acc);
}
