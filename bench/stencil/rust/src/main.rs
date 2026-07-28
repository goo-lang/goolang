// 1D heat-equation stencil — Rust with rayon. See ../stencil.goo for the
// algorithm and for why the checksum is weighted rather than a plain sum.
//
// FAIRNESS NOTE. rayon defaults to one worker per logical CPU, which is 32 on
// the measurement box. The algorithm is defined as EIGHT lanes, and the Goo
// and Go versions use eight. The pool is therefore pinned to eight so the
// comparison measures the same decomposition in all three languages rather
// than rewarding Rust for a wider default.
use rayon::prelude::*;

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

    for _ in 0..1000 {
        {
            let src: &[f64] = &cur;
            nxt.par_chunks_mut(chunk).enumerate().for_each(|(l, out)| {
                let lo = l * chunk;
                for (j, slot) in out.iter_mut().enumerate() {
                    let i = lo + j;
                    let left = if i > 0 { src[i - 1] } else { 0.0 };
                    let right = if i < n - 1 { src[i + 1] } else { 0.0 };
                    *slot = 0.25 * left + 0.5 * src[i] + 0.25 * right;
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
