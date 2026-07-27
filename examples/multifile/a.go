// File 1 of a TWO-file local package. Goo has never been able to compile a
// package from more than one file: resolve_package_dir (import_resolver.c)
// globs and sorts the whole directory correctly, but concat_package_sources
// (goo.c) then joins the files into one buffer, and parser.y's `program` rule
// admits exactly ONE package clause. A 2-file package therefore concatenates
// to `package p ... package p ...` and dies at the second clause.
//
// This file calls Bang(), which is declared in b.go — a cross-file reference
// inside one package, which is exactly what a single-file package cannot
// express and what every real Go program relies on.
package multifile

func Shout(s string) string {
	return s + Bang()
}
