# Claude Code Memory File

This file contains information about the project structure, tooling, and common commands to help Claude understand how to work with this codebase.

## Project Overview

This is the Goo programming language compiler project - a Go-compatible language with additional features like error unions, nullable types, ownership tracking, and advanced type systems.

## Task Management

This project uses `task-master` CLI for task management and project coordination.

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
- `make test` - Run tests
- `make clean` - Clean build artifacts
- `make test-reference` - Run reference manager tests
- `make test-flow` - Run flow analysis tests
- `make verify-core` - Full probe net, no CompCert required. Authoritative
  ccomp-free gate; safe for pre-push on any machine.
- `make verify` - `verify-core` plus the CompCert bootstrap pilot
  (`v2-bootstrap-pilot`); requires an opam CompCert switch.

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

## Key Features Implemented

- **Enhanced Interface System** (Task #22) including:
  - Automatic constraint inference
  - Concept-based generics
  - Higher-kinded types
  - Type-level programming
  - Protocol-oriented programming
- **Error unions** with `!` syntax
- **Nullable types** with `?` syntax
- **Ownership tracking** and move semantics
- **Channel operations** for concurrency
- **LLVM-based code generation**

## Recent Completed Tasks

- Task #22.4 - Type-Level Programming Capabilities
- Task #22.5 - Protocol-Oriented Programming System

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