package fwdref

// Fixture for forward-ref-probe. Triple is declared BEFORE the Double it calls,
// exercising intra-package forward references end to end: the type checker's
// two-pass signature hoist, the codegen prototype pre-pass, and package-mangled
// intra-package call resolution. Triple(14) = Double(14) + 14 = 28 + 14 = 42.
func Triple(x int) int { return Double(x) + x }

func Double(x int) int { return x + x }
