# Claude Code Memory File

This file contains information about the project structure, tooling, and common commands to help Claude understand how to work with this codebase.

## Project Overview

This is the Goo programming language compiler project - a Go-compatible language with additional features like error unions (`!T`), nullable types (`?T`), comptime metaprogramming, and opt-in arena memory regions.

## Task Management

This project uses `task-master` CLI for task management and project coordination.

**Status disclaimer (P5.9):** task-master statuses are historical planning
state, NOT the v1 definition of done. The 2026-07-08 audit found tasks
marked `done` whose own test files fail to parse (#12 pattern matching,
#14 GPU, #15 WASM). The v1 DoD is the roadmap's exit gates: a claim counts
only if a probe in `make verify-core` passes on it
(docs/2026-07-08-v1-roadmap.md).

### Task Master Commands

#### Project Setup & Configuration
- `task-master init [--name=<name>] [--description=<desc>] [-y]` - Initialize a new project with Task Master structure
- `task-master models` - View current AI model configuration and available models
- `task-master models --setup` - Run interactive setup to configure AI models
- `task-master models --set-main <model_id>` - Set the primary model for task generation
- `task-master models --set-research <model_id>` - Set the model for research operations
- `task-master models --set-fallback <model_id>` - Set the fallback model (optional)

#### Task Generation
- `task-master parse-prd --input=<file.txt> [--num-tasks=10]` - Generate tasks from a PRD document
- `task-master generate` - Create individual task files from tasks.json

#### Task Management
- `task-master list [--status=<status>] [--with-subtasks]` - List all tasks with their status
- `task-master set-status --id=<id> --status=<status>` - Update task status (pending, done, in-progress, review, deferred, cancelled)
- `task-master sync-readme [--with-subtasks] [--status=<status>]` - Export tasks to README.md with professional formatting
- `task-master update --from=<id> --prompt="<context>"` - Update multiple tasks based on new requirements
- `task-master update-task --id=<id> --prompt="<context>"` - Update a single specific task with new information
- `task-master update-subtask --id=<parentId.subtaskId> --prompt="<context>"` - Append additional information to a subtask
- `task-master add-task --prompt="<text>" [--dependencies=<ids>] [--priority=<priority>]` - Add a new task using AI
- `task-master remove-task --id=<id> [-y]` - Permanently remove a task or subtask

#### Subtask Management
- `task-master add-subtask --parent=<id> --title="<title>" [--description="<desc>"]` - Add a new subtask to a parent task
- `task-master add-subtask --parent=<id> --task-id=<id>` - Convert an existing task into a subtask
- `task-master remove-subtask --id=<parentId.subtaskId> [--convert]` - Remove a subtask (optionally convert to standalone task)
- `task-master clear-subtasks --id=<id>` - Remove all subtasks from specified tasks
- `task-master clear-subtasks --all` - Remove subtasks from all tasks

#### Task Analysis & Breakdown
- `task-master analyze-complexity [--research] [--threshold=5]` - Analyze tasks and generate expansion recommendations
- `task-master complexity-report [--file=<path>]` - Display the complexity analysis report
- `task-master expand --id=<id> [--num=5] [--research] [--prompt="<context>"]` - Break down tasks into detailed subtasks
- `task-master expand --all [--force] [--research]` - Expand all pending tasks with subtasks

#### Task Navigation & Viewing
- `task-master next` - Show the next task to work on based on dependencies
- `task-master show <id>` - Display detailed information about a specific task

#### Dependency Management
- `task-master add-dependency --id=<id> --depends-on=<id>` - Add a dependency to a task
- `task-master remove-dependency --id=<id> --depends-on=<id>` - Remove a dependency from a task
- `task-master validate-dependencies` - Identify invalid dependencies without fixing them
- `task-master fix-dependencies` - Fix invalid dependencies automatically

### Quick Start Workflow
1. `task-master next` - Find the next task to work on
2. `task-master set-status --id=<id> --status=in-progress` - Mark task as in progress
3. Work on the task
4. `task-master set-status --id=<id> --status=done` - Mark task as completed

## Build System

This project uses a Makefile for building:

- `make lexer` - Build the main compiler
- `make test` - Run tests (in-process unit suite + `test-cli` CLI discipline suite)
- `make clean` - Clean build artifacts
- `make test-reference` - Run reference manager tests
- `make test-flow` - Run flow analysis tests
- `make test-golden` / `make test-golden-o2` - Golden fixture suites (-O0/-O2).
  Parallel since P5.8: `GOLDEN_JOBS=<n>` overrides the default of nproc.
- `make verify-core` - Full probe net, no CompCert required. Authoritative
  ccomp-free gate; safe for pre-push on any machine.
- `make verify` - `verify-core` plus the CompCert bootstrap pilot
  (`v2-bootstrap-pilot`); requires an opam CompCert switch.

Note: `bin/goo` links only the reachable set (`GOO_OBJS`, P5.6). The full
`OBJS` list feeds the standalone test targets that exercise unlinked
frameworks (constraint inference, concept generics, HKT, flow, reference
manager) — those frameworks are NOT part of the shipped compiler.

## Stdlib model

Two layers, both gated by `scripts/check_stdlib_coverage.sh` in verify-core:

- **C shim packages** (`fmt`, `os`, `time`, `sync`, ...): declarative
  signature table in `src/types/shim_signatures.c`, implementations in the
  runtime archive (`src/runtime/`).
- **Vendored source packages** (`strings`, `strconv`, `unicode/utf8`, ...):
  real Goo/Go source under `goostd/`, resolved via GOOROOT (bare import =
  GOOROOT-then-local; `./name` = source-dir only).

## Memory model (v1 limitation)

v1 heap allocations are malloc with NO systematic reclamation — no GC, no
ownership-based freeing. Opt-in `arena { ... }` regions (with escape
analysis auto-promoting escapers) are the only bulk-free mechanism. This is
a documented v1 limitation; GC/ownership reclamation is post-v1
(docs/2026-07-08-v1-roadmap.md Post-v1).

## Project Structure

- `src/` - Source code
  - `lexer/` - Lexical analysis
  - `parser/` - Syntax analysis and AST generation  
  - `ast/` - Abstract syntax tree definitions
  - `types/` - Type system and type checking
  - `codegen/` - LLVM IR code generation
  - `runtime/` - Runtime system
  - `errors/` - Error handling
  - `test/` - Test framework
- `include/` - Header files
- `tests/` - Test files
- `examples/` - Example Goo programs
- `.taskmaster/` - Task management files

## Key Features (verified — every item is probe-gated in make verify-core)

- **Go-compatible core**: functions/methods/interfaces (method-set
  enforcement), structs + embedding (incl. qualified `sync.Mutex`),
  packages (shim + vendored + local), goroutines/channels/select/close,
  defer (incl. in-loop), switch/type-switch, slices/maps/strings, os.Args,
  Go-parity nil and exit semantics.
- **Error unions** `!T` with `try`/`catch` (incl. value-yielding `catch =>`)
- **Nullable types** `?T` with `if let` / nil comparison
- **Comptime** blocks/values and inference-only monomorphized generics
- **Arena regions** `arena { ... }` with escape-analysis auto-promotion
- **LLVM-based code generation** with real -O1/2/3 pipelines (differential
  gate proves -O2 IR differs and behavior matches)

The Task #22-era type-system frameworks (constraint inference, concept
generics, HKT, type-level programming, protocol-oriented programming) are
NOT in `bin/goo` (unlinked in P5.6, kept only behind standalone test
targets) — do not describe them as shipped features.

## Configuration Files

- `.taskmaster/config.json` - AI model configuration file (managed by models cmd)
- `.env` - API keys for AI providers (ANTHROPIC_API_KEY, etc.)
- `.cursor/mcp.json` - API keys for Cursor integration

## Grammar changes

- Any change to `src/parser/parser.y`, `src/parser/lexer_bridge.c`, or lexer token
  emission: use the **goo-grammar** skill (`.claude/skills/goo-grammar/`). Minimum bar
  even without the skill: `./scripts/grammar-tripwire.sh` must PASS (the exact counts
  recorded in `scripts/grammar-tripwire.sh`'s `EXPECTED_SR`/`EXPECTED_RR`) before AND
  after the change; any delta is stop-the-line (see the skill's conflict-ledger for
  the justified-delta procedure and the current baseline number).

## Language Standard

- We're using C23 in this project