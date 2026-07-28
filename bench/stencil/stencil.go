// 1D heat-equation stencil — Go reference. See stencil.goo for the algorithm
// and for why the checksum is weighted rather than a plain sum.
//
// This is the idiomatic Go answer: a double buffer, eight goroutines each
// owning a contiguous range, and a WaitGroup barrier between rounds. It is
// SIMPLER than the lanes halo exchange because Go has no way to prove the
// ranges are disjoint — the programmer asserts it and the race detector may
// or may not be run. That difference is the point of the comparison, not a
// handicap given to Go.
package main

import (
	"fmt"
	"sync"
)

func main() {
	n := 1 << 20
	cur := make([]float64, n)
	nxt := make([]float64, n)
	cur[n/2] = 1.0

	const lanes = 8
	chunk := n / lanes

	for round := 0; round < 1000; round++ {
		var wg sync.WaitGroup
		for l := 0; l < lanes; l++ {
			wg.Add(1)
			go func(l int) {
				defer wg.Done()
				lo := l * chunk
				hi := lo + chunk
				for i := lo; i < hi; i++ {
					left := 0.0
					if i > 0 {
						left = cur[i-1]
					}
					right := 0.0
					if i < n-1 {
						right = cur[i+1]
					}
					nxt[i] = 0.25*left + 0.5*cur[i] + 0.25*right
				}
			}(l)
		}
		wg.Wait()
		cur, nxt = nxt, cur
	}

	acc := 0.0
	for i := 0; i < n; i++ {
		acc += cur[i] * float64(i%97)
	}
	fmt.Println(int64(acc * 1000000000.0))
}
