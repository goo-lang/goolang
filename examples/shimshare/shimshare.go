// A local source package whose OWN import list names sync and time — and
// whose probe main imports the SAME two shims. That co-import is the point:
// sync_own_import_probe deliberately keeps main clear of sync/time to prove
// the seeding fix (M2-B1 no-masking rule), whereas this package exists to
// exercise the OPPOSITE case, where both sides import the same shim path and
// a by-value shim struct crosses the boundary in both directions.
package shimshare

import "sync"
import "time"

var mu sync.Mutex
var calls int

// A by-value time.Time crossing INTO the package.
func Stamp(t time.Time) int64 {
	mu.Lock()
	calls = calls + 1
	mu.Unlock()
	return t.UnixNano()
}

// A by-value time.Time crossing OUT of the package.
func Origin() time.Time {
	return time.Now()
}

func Calls() int { return calls }
