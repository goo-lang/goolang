// Package filepath manipulates slash-separated paths.
//
// Pure text handling, so it is a vendored Goo package like sort — nothing here
// touches the filesystem. Self-contained by design: no goostd package currently
// imports another vendored goostd package, and this one does not become the
// first.
//
// SCOPE. Slash-separated paths only, which is what Go's filepath does on Unix
// and what path/filepath's own doc calls the "slash-separated" case. Windows
// volume names and backslash separators are NOT handled, and that is a scope
// decision rather than an oversight — Goo targets Linux today
// (docs/2026-07-08-v1-roadmap.md).
//
// NOT here: Abs, Rel, Walk, Glob and Split. Abs and Walk need the filesystem,
// so they belong with a directory-listing design rather than with text. Glob
// needs pattern matching. None of them blocks a CLI tool, because the SHELL
// expands `tool *.txt` into os.Args before the program starts.
package filepath

// IsAbs reports whether the path is absolute.
func IsAbs(path string) bool {
	return len(path) > 0 && path[0] == '/'
}

// Clean returns the shortest equivalent path by lexical processing. It applies
// `.` and `..` without touching the filesystem, and collapses repeated
// separators.
//
// Two behaviours here are Go's and are easy to get wrong:
//   - `..` at the root of an ABSOLUTE path is discarded: Clean("/../a") is
//     "/a", not "/../a". The root has no parent.
//   - `..` at the front of a RELATIVE path is kept: Clean("../a") is "../a",
//     because the parent is unknown without the filesystem.
//
// The empty path cleans to ".", not to "".
func Clean(path string) string {
	if path == "" {
		return "."
	}
	rooted := path[0] == '/'

	// Output stack of surviving segments.
	segs := []string{}
	start := 0
	i := 0
	for i <= len(path) {
		atEnd := i == len(path)
		if !atEnd && path[i] != '/' {
			i = i + 1
			continue
		}
		seg := path[start:i]
		start = i + 1
		i = i + 1

		if seg == "" || seg == "." {
			continue
		}
		if seg == ".." {
			if len(segs) > 0 && segs[len(segs)-1] != ".." {
				segs = segs[:len(segs)-1]
				continue
			}
			// Nothing to pop. Keep it only when relative; an absolute path has
			// no parent above its root.
			if !rooted {
				segs = append(segs, "..")
			}
			continue
		}
		segs = append(segs, seg)
	}

	out := ""
	for j := 0; j < len(segs); j++ {
		if j > 0 {
			out = out + "/"
		}
		out = out + segs[j]
	}
	if rooted {
		out = "/" + out
	}
	if out == "" {
		return "."
	}
	return out
}

// Base returns the last element of the path. Trailing separators are removed
// first. Base("") is ".", and Base of a path that is all separators is "/".
func Base(path string) string {
	if path == "" {
		return "."
	}
	end := len(path)
	for end > 0 && path[end-1] == '/' {
		end = end - 1
	}
	if end == 0 {
		// The path was nothing but separators.
		return "/"
	}
	start := end
	for start > 0 && path[start-1] != '/' {
		start = start - 1
	}
	return path[start:end]
}

// Dir returns all but the last element of the path, cleaned. Dir of a path with
// no separator is ".", and Dir("/a") is "/".
func Dir(path string) string {
	i := len(path) - 1
	for i >= 0 && path[i] != '/' {
		i = i - 1
	}
	// i+1 KEEPS the separator, which is what makes Dir("/a") clean to "/"
	// rather than to ".".
	return Clean(path[:i+1])
}

// Ext returns the file name extension, including its leading dot, or "" when
// the last element has no dot.
//
// Go finds the LAST dot, so Ext("a.b.c") is ".c" and — the surprising one —
// Ext(".hidden") is ".hidden", because a leading dot is still the last dot in
// that element.
func Ext(path string) string {
	i := len(path) - 1
	for i >= 0 && path[i] != '/' {
		if path[i] == '.' {
			return path[i:]
		}
		i = i - 1
	}
	return ""
}

// Join joins the elements with separators and cleans the result. Empty
// elements are ignored, and Join with no non-empty element returns "".
func Join(elem ...string) string {
	out := ""
	for i := 0; i < len(elem); i++ {
		if elem[i] == "" {
			continue
		}
		if out == "" {
			out = elem[i]
		} else {
			out = out + "/" + elem[i]
		}
	}
	if out == "" {
		return ""
	}
	return Clean(out)
}
