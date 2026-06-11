#ifndef TYPE_LEVEL_INTERNAL_H
#define TYPE_LEVEL_INTERNAL_H

#include <stddef.h>
#include "interface_system.h"

// Shared representation for the type-level programming units
// (type_level_programming.c, type_level_dependent.c,
// type_level_eval.c). interface_system.h exposes TypeLevelNat only
// as an opaque typedef; the units need the body.

// Type-level natural number representation
struct TypeLevelNat {
    enum { NAT_ZERO, NAT_SUCC } kind;
    struct TypeLevelNat* predecessor; // For Succ(n), this is n
    size_t value;                     // Cached numeric value
};

struct TypePattern {
    TypePatternKind kind;
    char* name;                    // Variable or constructor name
    struct TypePattern** subpatterns; // Subpatterns for constructors
    size_t subpattern_count;
    Type* literal_type;            // For literal patterns
    struct TypePattern* next;
};

struct TypeFamilyCase {
    TypePattern* pattern;          // Pattern to match against
    TypeLevelComputation* result;  // Result computation
    struct TypeFamilyCase* next;   // Next case
};

struct TypeFamily {
    char* name;                    // Family name
    TypeVariable* parameters;     // Type parameters
    TypeFamilyCase* cases;        // List of cases
    TypeLevelComputation* default_case; // Default case
    size_t parameter_count;       // Number of parameters
};

struct PatternEnv {
    char** var_names;
    Type** var_types;
    size_t binding_count;
    size_t capacity;
};

#endif // TYPE_LEVEL_INTERNAL_H
