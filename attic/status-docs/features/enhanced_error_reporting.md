# Enhanced Error Reporting System

The Goo compiler includes a comprehensive error reporting system that provides clear, understandable error messages with detailed explanations, suggestions, and contextual help.

## Overview

The enhanced error reporting system transforms cryptic compiler errors into helpful, educational messages that guide developers toward solutions. It integrates seamlessly with the REPL for interactive error exploration and learning.

## Key Features

### 1. **Detailed Error Messages**
- Clear error descriptions with context
- Source location highlighting
- Error categorization and severity levels
- Comprehensive error codes for reference

### 2. **Interactive Error Exploration**
- REPL integration for live error analysis
- Error explanation system with examples
- Search functionality for error topics
- Help system with guided solutions

### 3. **Educational Error Explanations**
- "What it means" - Clear explanation of the error
- "How to fix it" - Step-by-step guidance
- "Common causes" - Typical scenarios that trigger the error
- Code examples showing correct and incorrect usage

### 4. **Smart Error Categorization**
- **Syntax Errors**: Grammar and structure issues
- **Type Errors**: Type system violations
- **Semantic Errors**: Logic and usage problems
- **Runtime Errors**: Execution-time issues
- **Internal Errors**: Compiler bugs and system issues

## REPL Integration

### Error Commands

The REPL provides comprehensive error management through the `:errors` command:

```goo
goo> :errors help
=== Available Error Help Topics ===

- unexpected_token: Syntax errors occur when the code doesn't follow Goo's grammar rules.
- type_mismatch: Type mismatch errors happen when a value of one type is used where another type is expected.
- undefined_variable: This error occurs when trying to use a variable, function, or type that hasn't been declared.
- duplicate_definition: Redefinition errors happen when you try to declare the same symbol twice in the same scope.
- invalid_expression: This error occurs when performing an operation that's not valid for the given types.
```

### Available Commands

#### `:errors show` / `:errors list`
Display all current errors with detailed information:

```goo
goo> :errors show
=== Error Report ===

error: Type mismatch in assignment
  --> test.goo:15:8
  Category: Type | Code: type_mismatch
  Hint: Consider casting the value or changing the variable type
  Suggestion: Cast the value explicitly: (int)value or ensure the types match.
```

#### `:errors summary`
Show a concise summary of current errors and warnings:

```goo
goo> :errors summary

=== Summary ===
2 errors, 1 warning
```

#### `:errors explain <error_code>`
Get detailed explanation for a specific error type:

```goo
goo> :errors explain type_mismatch
=== Error Explanation: type_mismatch ===

What it means:
  Type mismatch errors happen when a value of one type is used where another type is expected.

How to fix it:
  Cast the value explicitly: (int)value or ensure the types match.

Common causes:
  Assigning wrong type to variable, passing wrong argument type to function
```

#### `:errors search <query>`
Search through error explanations:

```goo
goo> :errors search type
=== Search Results for: "type" ===

- type_mismatch: Type mismatch errors happen when a value of one type is used where another type is expected.
- undefined_type: This error occurs when trying to use a variable, function, or type that hasn't been declared.
```

#### `:errors clear`
Clear all current errors (useful during development):

```goo
goo> :errors clear
All errors cleared.
```

## Error Categories and Codes

### Syntax Errors (2000-2999)
- **E2000 - Unexpected Token**: Unexpected syntax element
- **E2001 - Missing Semicolon**: Statement terminator missing
- **E2002 - Missing Closing Paren**: Unmatched parentheses
- **E2003 - Missing Closing Brace**: Unmatched braces
- **E2004 - Invalid Expression**: Malformed expression syntax

### Type Errors (3000-3999)
- **E3000 - Type Mismatch**: Value type doesn't match expected type
- **E3001 - Undefined Variable**: Variable used before declaration
- **E3002 - Undefined Type**: Type reference not found
- **E3003 - Invalid Cast**: Type conversion not allowed
- **E3004 - Incompatible Types**: Types cannot be used together
- **E3005 - Duplicate Definition**: Symbol defined multiple times

### Semantic Errors (4000-4999)
- **E4000 - Invalid Assignment**: Assignment violates language rules
- **E4001 - Unreachable Code**: Code that can never execute
- **E4002 - Missing Return**: Function missing return statement
- **E4003 - Invalid Access**: Access to private or protected member

## Usage Examples

### Basic Error Reporting

```goo
// This code has a type error
let x: int = "hello";  // E3000: Type mismatch

goo> :errors show
error: Type mismatch in assignment
  --> <repl>:1:14
  Category: Type | Code: type_mismatch
  Suggestion: Cast the value explicitly or change the variable type

goo> :errors explain type_mismatch
=== Error Explanation: type_mismatch ===

What it means:
  Type mismatch errors happen when a value of one type is used where another type is expected.

How to fix it:
  Cast the value explicitly: (int)value or ensure the types match.

Common causes:
  Assigning wrong type to variable, passing wrong argument type to function
```

### Interactive Development Workflow

```goo
goo> let x: int = "hello"
error: Type mismatch in assignment
  --> <repl>:1:14
  Category: Type | Code: type_mismatch

goo> :errors explain type_mismatch
[... explanation displayed ...]

goo> let x: int = 42
x: int = 42

goo> :errors summary

=== Summary ===
All clear!
```

### Error Search and Help

```goo
goo> :errors search undefined
=== Search Results for: "undefined" ===

- undefined_variable: This error occurs when trying to use a variable, function, or type that hasn't been declared.

goo> :errors help
[... full help topics list ...]
```

## Error Message Format

### Standard Error Format

```
[severity]: [description]
  --> [file]:[line]:[column]
  Category: [category] | Code: [error_code]
  Hint: [optional hint]
  Suggestion: [optional suggestion]
```

### Color-Coded Display

- **Errors**: Red highlighting for critical issues
- **Warnings**: Yellow highlighting for potential problems
- **Notes**: Blue highlighting for informational messages
- **Locations**: Bold highlighting for file paths and positions
- **Categories**: Cyan highlighting for error classification

## Error Explanation System

### Comprehensive Help Database

Each error type includes:

1. **Clear Description**: What the error means in plain language
2. **Root Cause Analysis**: Why this error typically occurs
3. **Solution Guidance**: Step-by-step fixing instructions
4. **Common Scenarios**: Typical situations that trigger the error
5. **Code Examples**: Correct vs. incorrect code patterns

### Example Error Explanation

```goo
goo> :errors explain undefined_variable
=== Error Explanation: undefined_variable ===

What it means:
  This error occurs when trying to use a variable, function, or type that hasn't been declared.

How to fix it:
  Check spelling, ensure the symbol is declared before use, or import the necessary module.

Common causes:
  Typos in variable names, using variables before declaration, missing imports

Examples:
  ✓ Correct:   let x = 42; print(x);
  ✗ Incorrect: print(undefinedVar);
```

## Integration with Development Tools

### REPL Development Workflow

1. **Code Entry**: Enter code in REPL
2. **Error Detection**: Compiler reports errors immediately
3. **Error Exploration**: Use `:errors` commands to understand issues
4. **Solution Application**: Apply suggested fixes
5. **Verification**: Confirm fixes resolve the problems

### Error Learning Path

1. **Encounter Error**: See detailed error message
2. **Get Explanation**: Use `:errors explain` for understanding
3. **Search Related**: Use `:errors search` for similar issues
4. **Apply Solution**: Follow guidance to fix the error
5. **Build Knowledge**: Learn from explanations for future reference

## Best Practices

### For Developers

1. **Read Error Messages Carefully**: Pay attention to location and category information
2. **Use Explanations**: Leverage `:errors explain` to understand unfamiliar errors
3. **Search for Patterns**: Use `:errors search` to find related error types
4. **Clear Regularly**: Use `:errors clear` to maintain clean development sessions
5. **Learn from Errors**: Use error explanations as learning opportunities

### For Error Resolution

1. **Start with Location**: Always check the exact line and column indicated
2. **Understand the Category**: Different categories require different approaches
3. **Read Suggestions**: Follow provided suggestions as starting points
4. **Check Common Causes**: Review typical scenarios that cause the error
5. **Apply Systematically**: Make one change at a time and retest

## Advanced Features

### Error Filtering
- Filter by severity level (errors, warnings, notes)
- Filter by category (syntax, type, semantic, etc.)
- Search functionality for specific patterns

### Error Statistics
- Track error patterns during development
- Identify frequently encountered error types
- Monitor error resolution progress

### Contextual Help
- Location-aware error suggestions
- Integration with hot reload for immediate feedback
- Performance impact analysis of error fixes

## Future Enhancements

### Planned Features

1. **Auto-Fix Suggestions**: Automated code corrections for simple errors
2. **Error Clustering**: Group related errors for batch resolution
3. **Learning Recommendations**: Personalized learning based on error patterns
4. **IDE Integration**: Rich error display in external editors
5. **Error Analytics**: Development pattern analysis and optimization suggestions

### Enhanced Explanations

1. **Interactive Examples**: Runnable code examples in explanations
2. **Video Tutorials**: Linked video explanations for complex errors
3. **Community Solutions**: User-contributed solutions and examples
4. **Progressive Disclosure**: Detailed explanations based on experience level

The enhanced error reporting system transforms error handling from a frustrating debugging experience into an educational opportunity, helping developers understand not just what went wrong, but why it happened and how to prevent similar issues in the future.