# Goo Compiler Architecture

## Overview

The Goo compiler is a multi-stage compiler that prioritizes correctness, safety verification, and optimization. It uses a modular architecture allowing incremental compilation and parallel processing.

## Compilation Pipeline

```
Source Code (.goo)
         ↓
    [Lexer] → Tokens
         ↓
    [Parser] → AST
         ↓
[Semantic Analysis] → Typed AST
         ↓
[Safety Verification] → Verified AST
         ↓
 [HIR Generation] → High-level IR
         ↓
 [MIR Generation] → Mid-level IR
         ↓
  [Optimization] → Optimized MIR
         ↓
[LLVM IR Generation] → LLVM IR
         ↓
 [LLVM Backend] → Machine Code
```

## Components

### 1. Frontend

#### Lexer

- **Input**: Source code
- **Output**: Token stream
- **Features**:
  - Unicode support
  - Position tracking
  - Error recovery
  - Incremental lexing

```rust
// Implementation structure
pub struct Lexer {
    input: &str,
    position: usize,
    current_char: Option<char>,
    tokens: Vec<Token>,
    errors: Vec<LexError>,
}

pub enum Token {
    // Keywords
    Let, Mut, Func, Struct, Enum, Interface,
    // Identifiers and literals
    Ident(String),
    IntLit(i128),
    FloatLit(f64),
    StringLit(String),
    // Operators
    Plus, Minus, Star, Slash,
    // Delimiters
    LeftParen, RightParen, LeftBrace, RightBrace,
    // Special
    Arrow, FatArrow, Question, Bang,
}
```

#### Parser

- **Input**: Token stream
- **Output**: Abstract Syntax Tree (AST)
- **Algorithm**: Recursive descent with Pratt parsing for expressions
- **Features**:
  - Error recovery
  - Incremental parsing
  - AST caching

```rust
pub enum AstNode {
    Function(FunctionDef),
    Struct(StructDef),
    Impl(ImplBlock),
    Expression(Expr),
    Statement(Stmt),
    // ...
}

pub struct FunctionDef {
    name: String,
    generics: Vec<GenericParam>,
    params: Vec<Param>,
    return_type: Option<Type>,
    body: Block,
    attributes: Vec<Attribute>,
}
```

### 2. Middle-End

#### Semantic Analysis

- **Type Checking**: Ensures type correctness
- **Name Resolution**: Resolves all identifiers
- **Lifetime Analysis**: Automatic lifetime inference
- **Effect Analysis**: Tracks side effects

```rust
pub struct SemanticAnalyzer {
    type_env: TypeEnvironment,
    symbol_table: SymbolTable,
    lifetime_inference: LifetimeInferencer,
    effect_tracker: EffectTracker,
}

impl SemanticAnalyzer {
    fn analyze_function(&mut self, func: &FunctionDef) -> Result<TypedFunction> {
        // Enter new scope
        self.symbol_table.push_scope();
        
        // Add parameters to scope
        for param in &func.params {
            self.symbol_table.define(param.name, param.ty);
        }
        
        // Type check body
        let typed_body = self.analyze_block(&func.body)?;
        
        // Verify return type
        self.verify_return_type(&typed_body, &func.return_type)?;
        
        // Exit scope
        self.symbol_table.pop_scope();
        
        Ok(TypedFunction { ... })
    }
}
```

#### Safety Verification

- **Memory Safety**: Proves no use-after-free, double-free
- **Thread Safety**: Detects data races
- **Bounds Checking**: Proves array accesses safe
- **Null Safety**: Ensures no null pointer dereferences

```rust
pub struct SafetyVerifier {
    smt_solver: Z3Solver,
    abstract_interpreter: AbstractInterpreter,
    dataflow_analyzer: DataflowAnalyzer,
}

impl SafetyVerifier {
    fn verify_memory_safety(&mut self, func: &TypedFunction) -> Result<()> {
        // Build control flow graph
        let cfg = build_cfg(func);
        
        // Run abstract interpretation
        let abstract_states = self.abstract_interpreter.analyze(&cfg);
        
        // Generate verification conditions
        let vcs = self.generate_vcs(&cfg, &abstract_states);
        
        // Solve with SMT
        for vc in vcs {
            if !self.smt_solver.prove(vc)? {
                return Err(SafetyError::MemoryUnsafe);
            }
        }
        
        Ok(())
    }
}
```

### 3. IR Layers

#### HIR (High-Level IR)

- Close to source language
- Preserves all type information
- Used for type checking and borrow checking

```rust
pub enum HirExpr {
    Literal(Literal),
    Variable(VarId),
    Call {
        func: Box<HirExpr>,
        args: Vec<HirExpr>,
        generic_args: Vec<Type>,
    },
    MethodCall {
        receiver: Box<HirExpr>,
        method: String,
        args: Vec<HirExpr>,
    },
    Field {
        base: Box<HirExpr>,
        field: String,
    },
    // ...
}
```

#### MIR (Mid-Level IR)

- Control flow graph representation
- SSA form
- Target for optimizations

```rust
pub struct MirFunction {
    blocks: Vec<BasicBlock>,
    locals: Vec<Local>,
    args: Vec<Local>,
    return_ty: Type,
}

pub struct BasicBlock {
    statements: Vec<Statement>,
    terminator: Terminator,
}

pub enum Statement {
    Assign(Place, Rvalue),
    StorageLive(Local),
    StorageDead(Local),
}

pub enum Terminator {
    Return,
    Goto(BlockId),
    If {
        cond: Operand,
        then_block: BlockId,
        else_block: BlockId,
    },
    Call {
        func: Operand,
        args: Vec<Operand>,
        destination: Option<(Place, BlockId)>,
    },
}
```

#### LLVM IR

- Final IR before machine code
- Leverages LLVM optimization passes

### 4. Optimization Pipeline

#### Analysis Passes

- **Alias Analysis**: Determines pointer relationships
- **Escape Analysis**: Identifies stack-allocatable objects
- **Dead Code Analysis**: Finds unreachable code
- **Constant Propagation**: Tracks compile-time values

#### Transformation Passes

- **Inlining**: Inline small functions
- **Loop Optimization**: Unrolling, vectorization
- **Memory Optimization**: Stack allocation, pooling
- **Devirtualization**: Convert dynamic to static dispatch

```rust
pub trait OptimizationPass {
    fn name(&self) -> &str;
    fn run(&mut self, mir: &mut MirFunction) -> bool;
}

pub struct InliningPass {
    inline_threshold: usize,
}

impl OptimizationPass for InliningPass {
    fn run(&mut self, mir: &mut MirFunction) -> bool {
        let mut changed = false;
        
        for block in &mut mir.blocks {
            if let Terminator::Call { func, args, destination } = &block.terminator {
                if self.should_inline(func) {
                    self.inline_call(mir, block, func, args, destination);
                    changed = true;
                }
            }
        }
        
        changed
    }
}
```

### 5. Code Generation

#### LLVM Integration

```rust
pub struct CodeGenerator {
    llvm_context: LLVMContext,
    llvm_module: LLVMModule,
    llvm_builder: LLVMBuilder,
    target_machine: LLVMTargetMachine,
}

impl CodeGenerator {
    fn generate_function(&mut self, func: &MirFunction) -> LLVMValueRef {
        // Create LLVM function
        let llvm_func = self.create_function_proto(func);
        
        // Generate basic blocks
        let mut blocks = HashMap::new();
        for (id, block) in func.blocks.iter().enumerate() {
            blocks.insert(id, self.create_basic_block(llvm_func, &format!("bb{}", id)));
        }
        
        // Generate instructions
        for (id, block) in func.blocks.iter().enumerate() {
            self.llvm_builder.position_at_end(blocks[&id]);
            self.generate_block(block, &blocks);
        }
        
        llvm_func
    }
}
```

#### Platform-Specific Optimization

- **x86_64**: AVX2/AVX-512 utilization
- **ARM**: NEON instructions
- **WebAssembly**: SIMD support

## Incremental Compilation

### Dependency Tracking

```rust
pub struct DependencyGraph {
    nodes: HashMap<ItemId, Node>,
    edges: HashMap<ItemId, Vec<ItemId>>,
}

pub struct Node {
    kind: NodeKind,
    fingerprint: Fingerprint,
    last_modified: SystemTime,
}

pub enum NodeKind {
    Function,
    Type,
    Constant,
    Module,
}
```

### Query-Based Architecture

- Fine-grained dependency tracking
- Minimal recompilation
- Parallel query execution

```rust
pub trait CompilerQuery {
    type Key;
    type Value;
    
    fn compute(&self, key: Self::Key) -> Self::Value;
    fn dependencies(&self, key: Self::Key) -> Vec<QueryId>;
}

pub struct TypeOfQuery;

impl CompilerQuery for TypeOfQuery {
    type Key = ExprId;
    type Value = Type;
    
    fn compute(&self, expr_id: ExprId) -> Type {
        // Compute type of expression
    }
}
```

## Error Handling

### Error Recovery

- Continue parsing after errors
- Provide multiple error messages
- Suggest fixes

### Error Quality

```rust
pub struct Diagnostic {
    level: DiagnosticLevel,
    message: String,
    location: SourceLocation,
    notes: Vec<Note>,
    suggestions: Vec<Suggestion>,
}

pub struct Suggestion {
    message: String,
    replacements: Vec<Replacement>,
}

pub struct Replacement {
    span: SourceSpan,
    text: String,
}

// Example error generation
fn type_mismatch_error(expected: &Type, found: &Type, location: SourceLocation) -> Diagnostic {
    Diagnostic {
        level: DiagnosticLevel::Error,
        message: format!("Type mismatch: expected {}, found {}", expected, found),
        location,
        notes: vec![
            Note {
                message: "Types must match exactly in this context".to_string(),
                location: None,
            }
        ],
        suggestions: vec![
            Suggestion {
                message: format!("Convert to {}", expected),
                replacements: vec![
                    Replacement {
                        span: location.to_span(),
                        text: format!("{}.into()", found),
                    }
                ],
            }
        ],
    }
}
```

## Performance Considerations

### Parallelization Points

- Lexing multiple files
- Type checking independent functions
- Optimization passes
- LLVM code generation

### Caching Strategy

- AST caching per file
- Type information caching
- HIR/MIR caching
- Incremental compilation cache

```rust
pub struct CompilerCache {
    ast_cache: HashMap<FileId, CachedAst>,
    type_cache: HashMap<ExprId, Type>,
    mir_cache: HashMap<FunctionId, MirFunction>,
}

pub struct CachedAst {
    ast: AstNode,
    fingerprint: Fingerprint,
    dependencies: Vec<FileId>,
}
```

## Implementation Language

The compiler is implemented in Rust for:

- Memory safety
- Performance
- Strong type system
- Excellent LLVM bindings

## Build Times Target

| Project Size | Debug Build | Release Build |
|-------------|-------------|---------------|
| Small (<1K LOC) | <100ms | <1s |
| Medium (<10K LOC) | <1s | <10s |
| Large (<100K LOC) | <10s | <60s |
| Huge (>100K LOC) | <30s | <3min |

## Diagnostics Example

```
Error: Cannot borrow `data` as mutable more than once

   --> src/main.goo:15:9
    |
15  |     let x = &mut data;
    |             --------- first mutable borrow occurs here
16  |     let y = &mut data;
    |             ^^^^^^^^^ second mutable borrow occurs here
17  |     use(x);
    |         - first borrow used here
    |
    = help: Consider using interior mutability with `Cell` or `RefCell`
    = help: Or restructure code to avoid multiple mutable borrows
    
Quick fix available: Restructure to sequential borrows [Apply]
```