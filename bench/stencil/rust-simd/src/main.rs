// 1D heat-equation stencil — Rust with rayon AND std::simd.
//
// NIGHTLY ONLY. std::simd is unstable, so this needs `cargo +nightly`. That
// matters for how any claim gets worded: a Goo result compared against THIS
// binary is compared against what Rust can do on nightly, not against what
// Rust ships to users on stable.
//
// The arithmetic keeps the same associativity as the scalar version —
// (left*0.25 + mid*0.5) + right*0.25 — and Rust does not contract to FMA by
// default, so the checksum must match the scalar builds bit-for-bit. It does.
// A mismatch here would mean the two are not the same computation and the
// timing comparison would be void.
#![feature(portable_simd)]
use rayon::prelude::*;
use std::simd::f64x8;

const W: usize = 8; // SIMD width in f64 lanes

fn main() {
    let n: usize = 1 << 20;
    let lanes: usize = 8;
    let chunk = n / lanes;

    rayon::ThreadPoolBuilder::new()
        .num_threads(lanes)
        .build_global()
        .unwrap();

    let mut cur = vec![0.0f64; n];
    let mut nxt = vec![0.0f64; n];
    cur[n / 2] = 1.0;

    let cl = f64x8::splat(0.25);
    let cm = f64x8::splat(0.5);
    let cr = f64x8::splat(0.25);

    for _ in 0..1000 {
        {
            let src: &[f64] = &cur;
            nxt.par_chunks_mut(chunk).enumerate().for_each(|(l, out)| {
                let lo = l * chunk;
                let olen = out.len();
                let mut j = 0usize;
                while j < olen {
                    let i = lo + j;
                    // Take the vector path only where the whole 8-wide window
                    // is interior. The domain edges fall back to scalar, which
                    // is two cells in the entire array.
                    if i >= 1 && i + W + 1 <= n && j + W <= olen {
                        let left = f64x8::from_slice(&src[i - 1..i - 1 + W]);
                        let mid = f64x8::from_slice(&src[i..i + W]);
                        let right = f64x8::from_slice(&src[i + 1..i + 1 + W]);
                        let r = left * cl + mid * cm + right * cr;
                        r.copy_to_slice(&mut out[j..j + W]);
                        j += W;
                    } else {
                        let left = if i > 0 { src[i - 1] } else { 0.0 };
                        let right = if i < n - 1 { src[i + 1] } else { 0.0 };
                        out[j] = 0.25 * left + 0.5 * src[i] + 0.25 * right;
                        j += 1;
                    }
                }
            });
        }
        std::mem::swap(&mut cur, &mut nxt);
    }

    let mut acc = 0.0f64;
    for i in 0..n {
        acc += cur[i] * ((i % 97) as f64);
    }
    println!("{}", (acc * 1000000000.0) as i64);
}
