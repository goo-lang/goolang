# Go test corpus (go1.27.4)

Origin: the `test/` directory of the Go distribution, version go1.27.4
(`/usr/local/go/test`). License: see `LICENSE` in this folder (Go BSD 3-Clause).

Selection rule: every file in `test/*.go` and `test/fixedbugs/*.go` whose first
line is exactly `// run`, plus the matching `.out` file where one exists.
Goo must accept these programs, because all valid Go is valid Goo.
`scripts/superset_probe.sh` measures how many it accepts at three levels
(parse, compile, behaviour). The ratchet files are in `tests/superset/`.

Regenerate this copy (from the repository root):

```sh
G=/usr/local/go; D=third_party/go-test-corpus/go1.27.4
rm -rf "$D"; mkdir -p "$D/fixedbugs"; cp "$G/LICENSE" "$D/"
for f in "$G"/test/*.go "$G"/test/fixedbugs/*.go; do
  [ "$(head -n1 "$f")" = "// run" ] || continue
  r=${f#"$G"/test/}; cp "$f" "$D/$r"
  [ -f "${f%.go}.out" ] && cp "${f%.go}.out" "$D/${r%.go}.out"
done
```
(This README is not part of the generated copy. Restore it after you regenerate.)
