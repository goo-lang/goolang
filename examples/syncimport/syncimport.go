// A local source package whose OWN import list names sync and time.
// Its probe main deliberately does NOT import sync/time — resolution
// must come from this package's own imports (M2-B1 no-masking rule).
package syncimport

import "sync"
import "time"

var mu sync.Mutex
var count int

func Bump() {
	mu.Lock()
	count = count + 1
	mu.Unlock()
}

func Count() int { return count }

func Nap() {
	time.Sleep(time.Millisecond)
}
