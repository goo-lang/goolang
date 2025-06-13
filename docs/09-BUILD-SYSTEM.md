# Goo Build System

## Overview

The Goo build system is designed to be fast, intelligent, and zero-configuration for most projects. It provides reproducible builds, excellent caching, and seamless cross-compilation support.

## Getting Started

### Zero Configuration

```bash
# In any directory with .goo files
$ goo build
[Auto-detected] Binary target: myapp
[Building] Compiling 3 modules...
[Linking] Creating executable...
[Success] Built ./myapp in 0.3s

$ goo run
[Running] ./myapp
Hello, World!
```

### Project Structure Detection

```
myproject/
├── main.goo          # Auto-detected as binary
├── lib.goo           # Auto-detected as library  
├── utils/
│   └── helper.goo    # Auto-included
└── tests/
    └── main_test.goo # Auto-detected as test
```

## Build Configuration

### goo.mod - Project Definition

```toml
# goo.mod
module github.com/user/myproject

version = "1.0.0"
authors = ["Your Name <you@example.com>"]
license = "MIT"

[dependencies]
http = "1.2.0"
json = { version = "2.0.0", features = ["streaming"] }
database = { git = "https://github.com/user/database", branch = "main" }

[dev-dependencies]
mockserver = "0.5.0"
bench = "1.0.0"

[build]
# Optional build configuration
target = "native"
opt-level = 3
lto = true
```

### Build Targets

#### Binary Targets

```toml
[[bin]]
name = "myapp"
src = "src/main.goo"

[[bin]]
name = "cli-tool"
src = "src/cli/main.goo"
required-features = ["cli"]
```

#### Library Targets

```toml
[lib]
name = "mylib"
type = "static"  # or "dynamic", "both"
src = "src/lib.goo"
public = ["src/api/*.goo"]  # Public API files
```

#### Test Configuration

```toml
[test]
parallel = true
timeout = "30s"
coverage = true

[[test]]
name = "integration"
src = "tests/integration/*.goo"
requires-network = true
```

## Build Commands

### Basic Commands

```bash
# Build the project
$ goo build
$ goo build --release           # Optimized build
$ goo build --target=wasm32     # Cross-compile

# Run the project
$ goo run
$ goo run --release -- arg1 arg2
$ goo run --example=demo

# Test the project
$ goo test
$ goo test --filter="auth*"     # Run specific tests
$ goo test --bench              # Run benchmarks
$ goo test --coverage           # Generate coverage

# Clean build artifacts
$ goo clean
$ goo clean --cache             # Also clean build cache
```

### Advanced Building

#### Conditional Compilation

```goo
// Build tags in source files
// +build linux,amd64

package platform

// +build !windows
func get_home_dir() string {
    return env("HOME")
}

// +build windows
func get_home_dir() string {
    return env("USERPROFILE")
}
```

#### Feature Flags

```bash
# Enable features
$ goo build --features="tls,compression"

# In code:
#[cfg(feature = "tls")]
import std.net.tls

#[cfg(feature = "compression")]
func compress_data(data: []byte) []byte {
    return zlib.compress(data)
}

#[cfg(not(feature = "compression"))]
func compress_data(data: []byte) []byte {
    return data  // No compression
}
```

## Dependency Management

### Adding Dependencies

```bash
# Add a dependency
$ goo add http
[Resolved] http@1.2.0
[Updated] goo.mod and goo.lock

# Add with specific version
$ goo add json@2.0.0

# Add from git
$ goo add github.com/user/lib

# Add as dev dependency
$ goo add --dev mockserver
```

### Dependency Resolution

```bash
$ goo deps
Package Tree:
myproject@1.0.0
├── http@1.2.0
│   ├── io@0.5.0
│   └── net@0.8.0
├── json@2.0.0
│   └── parser@1.0.0
└── database@3.0.0
    ├── sql@2.0.0
    └── pool@1.5.0

$ goo deps --outdated
Package    Current  Latest  Compatible
http       1.2.0    1.5.0   1.5.0
json       2.0.0    2.1.0   2.0.0 (breaking changes)
database   3.0.0    3.0.2   3.0.2
```

### Lock File (goo.lock)

```toml
# Automatically generated - do not edit
[[package]]
name = "http"
version = "1.2.0"
checksum = "sha256:abcdef..."
source = "registry+https://packages.goo-lang.org"

[[package]]
name = "json"
version = "2.0.0"
checksum = "sha256:123456..."
source = "registry+https://packages.goo-lang.org"
dependencies = ["parser@1.0.0"]
```

## Build Cache

### Intelligent Caching

```bash
$ goo build --explain-cache
Cache Analysis:
  ✓ main.goo (cached - unchanged)
  ✓ utils.goo (cached - unchanged)
  ✗ server.goo (rebuild - modified at 14:23:10)
  ✗ handlers.goo (rebuild - dependency changed)
  
Cache stats:
  Hit rate: 67%
  Size: 124MB
  Age: 3 days

$ goo cache
Cache Management:
  Size: 124MB / 1GB (12.4%)
  Projects: 5
  Artifacts: 1,234

$ goo cache clean --older-than=7d
[Cleaned] Removed 523 artifacts (45MB)
```

### Distributed Cache

```toml
[cache]
type = "distributed"
url = "https://cache.company.internal"
read-token = "${CACHE_READ_TOKEN}"
write-token = "${CACHE_WRITE_TOKEN}"

# Or S3-compatible
[cache]
type = "s3"
bucket = "goo-build-cache"
region = "us-east-1"
prefix = "team/project/"
```

## Cross-Compilation

### Target Triple

```bash
# List available targets
$ goo targets
Available targets:
  x86_64-unknown-linux-gnu      (host)
  x86_64-pc-windows-msvc
  x86_64-apple-darwin
  aarch64-unknown-linux-gnu
  aarch64-apple-darwin
  wasm32-unknown-unknown
  wasm32-wasi

# Cross-compile
$ goo build --target=aarch64-apple-darwin
[Cross-compiling] Target: aarch64-apple-darwin
[Success] Built ./myapp-aarch64-apple
```

### Multi-Architecture Builds

```bash
# Build for multiple targets
$ goo build --target=all
[Building] x86_64-linux...   ✓
[Building] x86_64-windows... ✓
[Building] x86_64-darwin...  ✓
[Building] aarch64-linux...  ✓
[Building] aarch64-darwin... ✓

Output:
  ./dist/myapp-x86_64-linux
  ./dist/myapp-x86_64-windows.exe
  ./dist/myapp-x86_64-darwin
  ./dist/myapp-aarch64-linux
  ./dist/myapp-aarch64-darwin
```

## Build Scripts

### build.goo - Custom Build Logic

```goo
// build.goo
import std.build.*

func build(ctx: BuildContext) !void {
    // Generate code before building
    try generate_bindings(ctx)
    
    // Add custom flags
    if ctx.target.os == "linux" {
        ctx.add_cflag("-DLINUX_SPECIFIC")
    }
    
    // Link native libraries
    ctx.link_library("sqlite3")
    
    // Embed resources
    ctx.embed_file("assets/config.toml")
    ctx.embed_dir("static/")
}

func generate_bindings(ctx: BuildContext) !void {
    let proto_files = ctx.glob("proto/*.proto")?
    
    for proto in proto_files {
        let out = proto.with_extension("goo")
        try protoc(proto, out)
        ctx.watch_file(proto)  // Rebuild if proto changes
    }
}
```

### Pre/Post Build Hooks

```toml
[hooks]
pre-build = ["goo run scripts/prebuild.goo"]
post-build = ["goo run scripts/postbuild.goo"]
pre-test = ["docker-compose up -d"]
post-test = ["docker-compose down"]
```

## Optimization Profiles

### Release Profiles

```toml
[profile.release]
opt-level = 3          # Maximum optimization
lto = true            # Link-time optimization
strip = true          # Strip symbols
panic = "abort"       # Smaller binary
codegen-units = 1     # Better optimization

[profile.release-with-debug]
inherits = "release"
strip = false
debug = true

[profile.bench]
inherits = "release"
lto = false          # Faster compilation for benchmarks
```

### Custom Profiles

```toml
[profile.production]
opt-level = 3
lto = "fat"          # Whole program optimization
panic = "unwind"     # Can recover from panics
overflow-checks = true
hardened = true      # Security hardening

# Use custom profile
$ goo build --profile=production
```

## Workspace Support

### Multi-Package Projects

```toml
# workspace.goo
[workspace]
members = [
    "core",
    "cli", 
    "server",
    "shared"
]

[workspace.dependencies]
# Shared dependencies for all members
tokio = "1.0"
serde = "1.0"
```

### Building Workspace

```bash
# Build all workspace members
$ goo build --workspace
[Building] core...    ✓
[Building] shared...  ✓
[Building] cli...     ✓
[Building] server...  ✓

# Build specific member
$ goo build -p server

# Run tests for all members
$ goo test --workspace
```

## Continuous Integration

### CI Configuration

```yaml
# .github/workflows/ci.yml
name: CI
on: [push, pull_request]

jobs:
  test:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, windows-latest, macos-latest]
    
    steps:
    - uses: actions/checkout@v2
    
    - name: Install Goo
      uses: goo-lang/setup-goo@v1
      with:
        version: stable
    
    - name: Cache
      uses: actions/cache@v2
      with:
        path: ~/.goo/cache
        key: ${{ runner.os }}-goo-${{ hashFiles('**/goo.lock') }}
    
    - name: Build
      run: goo build --release
    
    - name: Test
      run: goo test --coverage
    
    - name: Upload coverage
      uses: codecov/codecov-action@v1
```

### Build Matrix

```toml
# ci.toml
[[matrix]]
os = ["ubuntu", "windows", "macos"]
arch = ["x86_64", "aarch64"]
profile = ["debug", "release"]

[[matrix.exclude]]
os = "windows"
arch = "aarch64"

[ci]
parallel = true
fail-fast = false
timeout = "30m"
```

## Package Publishing

### Publishing a Package

```bash
# Login to package registry
$ goo login
Username: myuser
Password: 
[Success] Logged in as myuser

# Publish package
$ goo publish
[Checking] Running tests...        ✓
[Checking] Verifying metadata...   ✓
[Checking] Building package...     ✓
[Checking] Validating license...   ✓
[Publishing] Uploading mylib v1.0.0...
[Success] Published to https://packages.goo-lang.org/mylib

# Publish with dry run
$ goo publish --dry-run
```

### Package Metadata

```toml
[package]
name = "awesome-lib"
version = "1.0.0"
authors = ["Your Name <you@example.com>"]
edition = "2024"
description = "An awesome library"
readme = "README.md"
homepage = "https://github.com/you/awesome-lib"
repository = "https://github.com/you/awesome-lib"
license = "MIT OR Apache-2.0"
keywords = ["awesome", "library"]
categories = ["web", "async"]

[package.metadata.docs]
features = ["all"]
```

## Build Performance

### Incremental Compilation

```bash
$ goo build --timings
Build Timings:
  Parsing:     12ms  (3 files)
  Type Check:  45ms  (3 modules)
  Codegen:     23ms  (1 module changed)
  Optimize:    67ms
  Link:        34ms
  Total:       181ms

Incremental stats:
  Cached:      12/15 modules (80%)
  Rebuilt:     3/15 modules (20%)
  Speedup:     8.2x vs clean build
```

### Parallel Building

```bash
# Automatic parallelization
$ goo build
[Building] Using 16 threads (8 cores × 2)
[Progress] ████████████████░░░░ 14/20 modules

# Control parallelism
$ goo build -j 4           # Use 4 threads
$ goo build -j 1           # Serial build
```

## Docker Integration

### Dockerfile Generation

```bash
$ goo docker init
[Generated] Dockerfile
[Generated] .dockerignore

# Multi-stage Dockerfile created:
FROM goo:1.0-alpine AS builder
WORKDIR /app
COPY . .
RUN goo build --release

FROM alpine:latest
COPY --from=builder /app/target/release/myapp /usr/local/bin/
CMD ["myapp"]
```

### Container Builds

```bash
# Build in container
$ goo docker build
[Building] Creating build container...
[Building] Compiling in container...
[Success] Image created: myapp:latest

# Run in container
$ goo docker run
[Running] Starting container...
```

## Security

### Build Security

```toml
[security]
# Require signed dependencies
require-signatures = true

# Checksum verification
verify-checksums = true

# Audit dependencies
audit-on-build = true

# Reproducible builds
reproducible = true
```

### Security Scanning

```bash
$ goo audit
Security Audit:
  Scanning 45 dependencies...
  
  Found 2 vulnerabilities:
  
  CRITICAL: json@1.0.0
    CVE-2024-1234: Remote code execution
    Fixed in: 1.0.1
    Action: Run 'goo update json'
  
  MODERATE: http@0.9.0
    CVE-2024-5678: DoS vulnerability
    Fixed in: 1.0.0
    Action: Run 'goo update http'

$ goo audit --fix
[Updating] json@1.0.0 → json@1.0.1
[Updating] http@0.9.0 → http@1.0.0
[Success] All vulnerabilities fixed
```

## Advanced Features

### Build Plugins

```goo
// plugins/custom.goo
import std.build.plugin

plugin CustomPlugin {
    name = "custom-generator"
    version = "1.0.0"
    
    func execute(ctx: PluginContext) !void {
        // Custom build logic
        for file in ctx.source_files() {
            if file.ends_with(".proto") {
                generate_proto(file)
            }
        }
    }
}
```

### Build Configuration

```toml
[build]
# Use custom linker
linker = "lld"

# Custom LLVM passes
llvm-passes = ["mem2reg", "dce", "inline"]

# Target-specific settings
[build.target.wasm32]
features = ["simd", "bulk-memory"]
link-args = ["--no-entry"]

[build.target.windows]
link-args = ["/SUBSYSTEM:WINDOWS"]
```

### Performance Monitoring

```bash
$ goo build --profile-build
[Profiling] Build performance analysis...

Build Hotspots:
  34% │ Type checking     @ type_checker.rs:234
  28% │ LLVM codegen      @ codegen.rs:567
  21% │ Dependency parse  @ parser.rs:123
  17% │ Link time         @ linker.rs:89

Suggestions:
1. Enable parallel type checking: --features=parallel-typecheck
2. Use thin LTO for faster incremental builds: lto = "thin"
3. Reduce dependency parsing with lock file optimization
```

## Summary

The Goo build system provides:

1. **Zero Configuration**: Works out of the box for most projects
2. **Fast Builds**: Incremental compilation, intelligent caching
3. **Cross-Compilation**: Simple multi-platform builds
4. **Dependency Management**: Modern, secure package management
5. **Reproducible Builds**: Consistent builds across environments
6. **Excellent Tooling**: Integration with CI/CD, Docker, IDEs
7. **Security First**: Vulnerability scanning, signed packages
8. **Performance Focus**: Parallel builds, optimization profiles
9. **Extensibility**: Custom build scripts, plugins, hooks

The goal is to make building Goo projects fast, reliable, and painless, allowing developers to focus on writing code rather than fighting build tools.