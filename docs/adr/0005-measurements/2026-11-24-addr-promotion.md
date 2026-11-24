# Cost of escaping-address promotion (audit finding C1)

Taken 2026-11-24 on branch `fix/audit-addr-escape` @ `19421d5b`, against
`origin/main` @ `d2aa8864` as the control. Both builds ran in ONE sitting.

## What the change does

A local goes to the heap, allocated at its declaration, when its address
escapes. Three triggers:

1. an explicit `&x` that `local_escape` marks as escaping;
2. a pointer-receiver call whose `param_escape` summary says the receiver or
   the result escapes;
3. the enclosing function has a `go` statement, or has no verdict (a
   parameter, a function literal).

A loop gets one slot for each iteration. No release path frees a promoted slot.

## Commands

```bash
W=<scratch>   # both output dirs have the same length; argv[0] is copied to the heap
GOOROOT=<main build dir>  <main build>/bin/goo -O2 -o $W/ma/daemon bench/daemon/daemon.goo
GOOROOT=$PWD              ./bin/goo            -O2 -o $W/br/daemon bench/daemon/daemon.goo
/usr/bin/time -f '%M' $W/XX/daemon 400000        # peak RSS in KB
valgrind $W/XX/daemon 2000                       # in use at exit
```

## Daemon benchmark: no change

`bench/daemon/daemon.goo` has no `&` and no address-taking receiver call.

| Build | In use at exit, 2,000 req | Blocks | Peak RSS, 400,000 req (two runs) | Output |
|---|---|---|---|---|
| `origin/main` d2aa8864 | 82,204 bytes | 4,003 | 26,036 / 27,204 KB | `23` |
| branch 19421d5b | 82,204 bytes | 4,003 | 26,744 / 26,756 KB | `23` |

Block delta: 0. Valgrind: 0 errors in both. The peak RSS difference lies
inside the run-to-run spread of the main build (1,168 KB), so it is noise.
This is one program, so it says nothing about code that takes addresses.

## Positive control: the cost exists when an address escapes

`f` stores `&x` of its local in a global, and `main` calls `f` 1,000 times
(`-O0`, valgrind, in use at exit).

| Build | Blocks | Bytes | Program output |
|---|---|---|---|
| `origin/main` | 0 | 0 | `0` (wrong: stack slot dangled) |
| branch | 1,000 | 24,000 | `999` (correct) |

Each escaping address-taken local costs one heap block (24 bytes here) for
each execution of its declaration, and the block is never freed. The old
`0 blocks` was a dangling pointer, not a saving.
