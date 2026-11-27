// Command vendor_go renames Goo keywords that Go source uses as identifiers.
//
// usage: go run tools/vendor_go/main.go -keywords a,b -out <dir> <file.go>...
//
// All files of one package go in one call, because a package-level name must
// be renamed the same way in every file. The tool writes each renamed file
// into <dir> under its base name and prints one line per rename on stdout:
//
//	<file>\t<old>-><new>@<line>:<col>
//
// The line and the column (a byte column, counted from 1) point into the
// ORIGINAL file, so a reader can apply the rows to the pristine bytes.
//
// WHY the parser: only identifiers change. Strings, comments and struct tags
// are not identifiers, so go/parser never reports them. go/scanner is a second
// witness: the count of keyword-named IDENT tokens must equal the count of
// keyword-named identifiers in the syntax tree, or the tool refuses.
package main

import (
	"flag"
	"fmt"
	"go/ast"
	"go/parser"
	"go/scanner"
	"go/token"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// keywords is filled from -keywords, which tools/vendor_go.py reads from the Goo
// lexer table (tools/goo_reserved.py), so the set has one source.
var keywords = map[string]bool{}

type edit struct {
	off       int
	line, col int
	old       string
}

func main() {
	out := flag.String("out", "", "directory for the renamed files")
	kw := flag.String("keywords", "", "comma-separated Goo reserved words to rename")
	flag.Parse()
	for _, k := range strings.Split(*kw, ",") {
		if k != "" {
			keywords[k] = true
		}
	}
	if *out == "" || len(keywords) == 0 || flag.NArg() == 0 {
		fmt.Fprintln(os.Stderr, "usage: vendor_go -out <dir> <file.go>...")
		os.Exit(2)
	}
	if err := run(*out, flag.Args()); err != nil {
		fmt.Fprintln(os.Stderr, "vendor_go:", err)
		os.Exit(1)
	}
}

func run(outDir string, files []string) error {
	fset := token.NewFileSet()
	type unit struct {
		path  string
		src   []byte
		edits []edit
	}
	var units []*unit
	taken := map[string]bool{} // every identifier of the package, for the collision check
	for _, p := range files {
		src, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		f, err := parser.ParseFile(fset, p, src, parser.SkipObjectResolution)
		if err != nil {
			return err
		}
		u := &unit{path: p, src: src}
		imports := map[string]bool{}
		for _, im := range f.Imports {
			if im.Name != nil {
				imports[im.Name.Name] = true
			}
		}
		// A selector on an imported package names an exported symbol, which
		// cannot be a lowercase keyword. Refuse instead of guessing.
		var visitErr error
		skip := map[*ast.Ident]bool{}
		ast.Inspect(f, func(n ast.Node) bool {
			switch x := n.(type) {
			case *ast.Ident:
				taken[x.Name] = true
				if !keywords[x.Name] || skip[x] {
					return true
				}
				if x == f.Name {
					visitErr = fmt.Errorf("%s: the package name %q is a Goo keyword", p, x.Name)
					return false
				}
				pos := fset.Position(x.Pos())
				u.edits = append(u.edits, edit{off: pos.Offset, line: pos.Line, col: pos.Column, old: x.Name})
			case *ast.FuncDecl:
				// A body-less function was supplied by an assembly file.
				if x.Body == nil {
					fmt.Printf("%s\tbodyless:%s\n", filepath.Base(p), x.Name.Name)
				}
			case *ast.SelectorExpr:
				if id, ok := x.X.(*ast.Ident); ok && imports[id.Name] && keywords[x.Sel.Name] {
					visitErr = fmt.Errorf("%s: selector %s.%s names a keyword in another package", p, id.Name, x.Sel.Name)
				}
			}
			return true
		})
		if visitErr != nil {
			return visitErr
		}
		// Second witness: the scanner must see the same identifiers.
		var s scanner.Scanner
		tf := fset.AddFile(p+"#scan", -1, len(src))
		s.Init(tf, src, nil, 0)
		n := 0
		for {
			_, tok, lit := s.Scan()
			if tok == token.EOF {
				break
			}
			if tok == token.IDENT && keywords[lit] {
				n++
			}
		}
		if n != len(u.edits) {
			return fmt.Errorf("%s: scanner saw %d keyword identifiers, parser saw %d", p, n, len(u.edits))
		}
		units = append(units, u)
	}
	for _, u := range units {
		for _, e := range u.edits {
			if taken[e.old+"_"] {
				return fmt.Errorf("%s:%d:%d: cannot rename %s, the package already uses %s_", u.path, e.line, e.col, e.old, e.old)
			}
		}
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return err
	}
	for _, u := range units {
		// Apply from the end so earlier offsets stay valid.
		sort.Slice(u.edits, func(i, j int) bool { return u.edits[i].off > u.edits[j].off })
		dst := u.src
		for _, e := range u.edits {
			if string(dst[e.off:e.off+len(e.old)]) != e.old {
				return fmt.Errorf("%s:%d:%d: offset does not hold %q", u.path, e.line, e.col, e.old)
			}
			dst = append(dst[:e.off:e.off], append([]byte(e.old+"_"), dst[e.off+len(e.old):]...)...)
		}
		if err := os.WriteFile(filepath.Join(outDir, filepath.Base(u.path)), dst, 0o644); err != nil {
			return err
		}
		sort.Slice(u.edits, func(i, j int) bool { return u.edits[i].off < u.edits[j].off })
		for _, e := range u.edits {
			fmt.Printf("%s\t%s->%s_@%d:%d\n", filepath.Base(u.path), e.old, e.old, e.line, e.col)
		}
	}
	return nil
}
