// First file of a two-file package. Calls Second(), declared in b.go.
package pkg_multifile_dir

func First() string {
	return "first+" + Second()
}
