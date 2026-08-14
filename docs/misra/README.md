# MISRA C:2012 adoption policy

This directory records which MISRA C:2012 guidelines the Goo compiler adopts,
which it does not, and why. It is the input to a build gate, not a
certification artefact.

Source: MISRA C:2012, Third edition, first revision (February 2019), 17
directives and 156 rules. Guideline headline text is quoted from a licensed
copy solely to identify the guideline under discussion.

## This is not a compliance claim

MISRA C:2012 section 5.5 sets the bar for claiming compliance. This project
does not meet it, and does not try to:

- **The language is out of scope.** MISRA C:2012 covers C90 and C99 (section
  3.1). This compiler is built with `-std=c23`. A later MISRA edition covers
  C11/C18; this one does not.
- **The target is out of scope.** MISRA addresses embedded critical systems on
  a freestanding implementation. `bin/goo` is a hosted development tool.
- **Coverage is partial.** The checking tool implements 130 of the 156 rules
  and none of the 17 directives.

What this policy *is*: a deliberate subset of MISRA's defect-class guidance,
adopted because it catches bugs that make a compiler wrong, with an honest
record of everything deliberately left out.

## Summary

| | Count |
|---|---|
| Guidelines in this edition | 173 (156 rules + 17 directives) |
| **Adopted** | **142** |
| &nbsp;&nbsp;- already clean, held by the gate | 55 |
| &nbsp;&nbsp;- enforced by review, sanitizer or fuzzer | 37 |
| &nbsp;&nbsp;- adopted with a fix backlog | 50 |
| Deviated (Required, needs a record) | 11 |
| Declined (Advisory, MISRA 6.2.3) | 20 |
| Violations to fix | 8,675 |
| Violations waived by deviation or decline | 20,997 |

Baseline scan: 29,679 violations in hand-written code = 8,675 to fix + 20,997 waived
+ 7 outside this edition.
No Mandatory guideline is violated, which is what makes the rest of this
policy possible: MISRA 6.2.1 permits no deviation from a Mandatory guideline.

## Only 30% of these violations are in code that ships

`bin/goo` links 57 of the 147 `.c` files under `src/`. The rest is the Task
#22-era framework code that P5.6 unlinked — constraint inference, concept
generics, HKT, proof obligations, the IPFS client — kept only behind standalone
test targets. Measured 2026-08-14:

| Where | Violations | Share |
|---|---|---|
| Files linked into `bin/goo` | 9,205 | 30.0% |
| Files **not** linked | 20,329 | 66.2% |
| Headers | 1,189 | 3.9% |

**Two thirds of the backlog is in code the project has already decided not to
ship.** Fix the shipped set first. Phase 1 restricted to shipped code and
headers is **170 violations** excluding Rule 17.7 — a single sitting, and it is
the highest-value slice of the whole exercise.

Reproduce the split with:

    # the informational LLVM banner shares the line, so filter to paths
    make --eval='__p: ; @echo $(GOO_SRCS)' __p | tr ' ' '\n' | grep '\.c$'

## How the decision was made

A guideline is adopted when breaking it can produce **undefined behaviour, a
wrong compilation result, memory corruption, or a leaked resource**. A
guideline is refused when it encodes embedded-C house style, or when it
forbids something this compiler is built out of.

The three big refusals all fall in the second group. Dynamic memory, recursion
and the AST downcast are not incidental habits; they are the architecture. See
D-01, D-05, D-06 and D-11.

## Project deviations (Required guidelines not followed)

MISRA 5.4 requires a formal record for each. One file each, in `deviations/`, using the Appendix I format.

| ID | Guideline | Waived | Reason |
|---|---|---|---|
| [D-01](deviations/D-01.md) | Rule 21.3 - The memory allocation and deallocation functions of <stdlib.h> shall not be used | 2,597 | malloc/free is the memory model: ARC, arenas, xalloc |
| [D-02](deviations/D-02.md) | Rule 17.7 - The value returned by a function having non-void return type shall be used | 1,709 | 1,709 waived by D-02 (printf and mem/str family); 1,516 kept, 840 of them an unchecked pthread_* return |
| [D-03](deviations/D-03.md) | Rule 14.4 - The controlling expression of an if statement and the controlling expression of an iteration-statement shall have essentially Boolean type | 2,111 | if (ptr) and if (!x) are the house idiom |
| [D-04](deviations/D-04.md) | Rule 21.6 - The Standard Library input/output functions shall not be used | 1,372 | a compiler must print diagnostics |
| [D-05](deviations/D-05.md) | Rule 11.3 - A cast shall not be performed between a pointer to object type and a pointer to a different object type | 912 | the AST downcast (CallExprNode *)node is the architecture |
| [D-06](deviations/D-06.md) | Rule 17.2 - Functions shall not call themselves, either directly or indirectly | 805 | recursive descent parser and AST walks |
| [D-07](deviations/D-07.md) | Rule 17.1 - The features of <stdarg.h> shall not be used | 50 | varargs diagnostic printers |
| [D-08](deviations/D-08.md) | Rule 21.10 - The Standard Library time and date functions shall not be used | 47 | the compiler measures and reports time |
| [D-09](deviations/D-09.md) | Rule 21.8 - The Standard Library functions abort, exit, getenv and system of <stdlib.h> shall not be used | 14 | exit() carries the compiler's exit status |
| [D-10](deviations/D-10.md) | Rule 21.4 - The standard header file <setjmp.h> shall not be used | 3 | setjmp/longjmp implements t.Fatal in goo test |
| [D-11](deviations/D-11.md) | Dir 4.12 - Dynamic memory allocation shall not be used | 0 | dynamic memory allocation is the memory model |

## Declined advisory guidelines

MISRA 6.2.3 does not require a formal deviation for an advisory guideline, but it does ask that non-compliance be documented. This table is that record.

| Guideline | Waived | Reason |
|---|---|---|
| Rule 15.5 - A function should have a single point of exit at the end | 6,858 | single exit; CLAUDE.md prefers early returns |
| Rule 12.1 - The precedence of operators within expressions should be made explicit | 1,463 | explicit precedence; style, no defect class |
| Rule 11.5 - A conversion should not be performed from pointer to void into pointer to object | 1,216 | void* to object* is every allocator result |
| Rule 8.7 - Functions and objects should not be defined with external linkage if they are referenced in only one translation unit | 1,197 | 1,197 sites; revisit as a link-surface cleanup |
| Rule 18.4 - The +, -, += and -= operators should not be applied to an expression of pointer type | 170 | pointer arithmetic is normal in a lexer and parser |
| Rule 13.3 - A full expression containing an increment (++) or decrement (--) operator should have no other potential side effects other than that caused by the increment or decrement operator | 148 | ++ with other side effects |
| Rule 5.9 - Identifiers that define objects or functions with internal linkage should be unique | 75 | internal linkage identifier uniqueness |
| Rule 19.2 - The union keyword should not be used | 55 | unions carry the AST and value representation |
| Rule 17.8 - A function parameter should not be modified | 53 | parameter modified |
| Rule 20.10 - The # and ## preprocessor operators should not be used | 44 | # and ## build the diagnostic and probe macros |
| Rule 12.3 - The comma operator should not be used | 41 | comma operator |
| Rule 8.9 - An object should be defined at block scope if its identifier only appears in a single function | 24 | block scope for single-use objects |
| Rule 18.5 - Declarations should contain no more than two levels of pointer nesting | 14 | more than two pointer levels |
| Rule 15.4 - There should be no more than one break or goto statement used to terminate any iteration statement | 7 | more than one break or goto per loop |
| Rule 20.1 - #include directives should only be preceded by preprocessor directives or comments | 6 | #include preceded by other than directives |
| Rule 11.4 - A conversion should not be performed between a pointer to object and an integer type | 4 | pointer to integer conversion |
| Rule 20.5 - #undef should not be used | 2 | #undef |
| Dir 4.6 - typedefs that indicate size and signedness should be used in place of the basic numerical types | 0 | int and size_t are the house types, not sized typedefs |
| Dir 4.8 - If a pointer to a structure or union is never dereferenced within a translation unit, then the implementation of the object should be hidden | 0 | opaque types would fight the AST downcast pattern |
| Dir 4.9 - A function should be used in preference to a function-like macro where they are interchangeable | 0 | the probe and diagnostic macros need to be macros |

## Fix backlog

### Phase 1 - undefined behaviour and real defect classes

Every rule here marks code that is either undefined, or a defect class this project has already been bitten by. It splits in two. Twenty-six rules carry 357 violations between them, which is an afternoon of work. Rule 17.7 carries the other 1,516 on its own, 840 of them an unchecked `pthread_*` return value, and that one is a project in itself. Do the 357 first.

27 rules, 1,873 violations to fix.

| Guideline | Category | To fix | Note |
|---|---|---|---|
| Rule 17.7 - The value returned by a function having non-void return type shall be used | Required | 1,516 | 1,709 waived by D-02 (printf and mem/str family); 1,516 kept, 840 of them an unchecked pthread_* return |
| Rule 11.8 - A cast shall not remove any const or volatile qualification from the type pointed to by a pointer | Required | 67 | free((void*)x) on a const char* field; a real defect class |
| Rule 12.2 - The right hand operand of a shift operator shall lie in the range zero to one less than the width in bits of the essential type of the left hand operand | Required | 45 | shift out of range is undefined behaviour |
| Rule 4.1 - Octal and hexadecimal escape sequences shall be terminated | Required | 41 | unterminated escape sequence |
| Rule 20.7 - Expressions resulting from the expansion of macro parameters shall be enclosed in parentheses | Required | 39 | unparenthesised macro parameter |
| Rule 16.4 - Every switch statement shall have a default label | Required | 26 | a missing default hides a new enum case; the sites switch on kind tags such as comptime value->type |
| Rule 22.8 - The value of errno shall be set to zero prior to a call to an errno-setting- function | Required | 18 | errno not cleared before the call |
| Rule 22.9 - The value of errno shall be tested against zero after calling an errno- setting-function | Required | 18 | errno not tested after the call |
| Rule 13.4 - The result of an assignment operator should not be used | Advisory | 14 | if (x = y) is a real defect class |
| Rule 11.6 - A cast shall not be performed between pointer to void and an arithmetic type | Required | 12 | void pointer to arithmetic type |
| Rule 16.3 - An unconditional break statement shall terminate every switch-clause | Required | 12 | missing break; fall-through must be explicit |
| Rule 7.4 - A string literal shall not be assigned to an object unless the object's type is "pointer to const-qualified char" | Required | 11 | string literal assigned to a non-const object |
| Rule 21.7 - The Standard Library functions atof, atoi, atol and atoll functions of <stdlib.h> shall not be used | Required | 11 | atoi/atof cannot report an error; use strtol |
| Rule 21.1 - #define and #undef shall not be used on a reserved identifier or reserved macro name | Required | 9 | #define on a reserved identifier is undefined |
| Rule 3.1 - The character sequences /* and // shall not be used within a comment | Required | 6 | /* inside a comment |
| Rule 16.2 - A switch label shall only be used when the most closely-enclosing compound statement is the body of a switch statement | Required | 5 | switch label not in the enclosing compound statement |
| Rule 21.16 - The pointer arguments to the Standard Library function memcmp shall point to either a pointer type, an essentially signed type, an essentially unsigned type, an essentially Boolean type or an essentially enum type | Required | 5 | memcmp on pointers to different types |
| Rule 7.1 - Octal constants shall not be used | Required | 4 | octal constant |
| Rule 2.2 - There shall be no dead code | Required | 3 | dead code |
| Rule 11.1 - Conversions shall not be performed between a pointer to a function and any other type | Required | 2 | function pointer conversion is undefined |
| Rule 21.15 - The pointer arguments to the Standard Library functions memcpy, memmove and memcmp shall be pointers to qualified or unqualified versions of compatible types | Required | 2 | memcpy pointer argument types |
| Rule 22.10 - The value of errno shall only be tested when the last function to be called was an errno-setting-function | Required | 2 | errno tested after a non-errno-setting call |
| Rule 9.3 - Arrays shall not be partially initialized | Required | 1 | partially initialized array |
| Rule 15.1 - The goto statement should not be used | Advisory | 1 | one goto; remove it |
| Rule 15.3 - Any label referenced by a goto statement shall be declared in the same block, or in any block enclosing the goto statement | Required | 1 | goto label scope |
| Rule 16.1 - All switch statements shall be well-formed | Required | 1 | switch not well-formed |
| Rule 20.12 - A macro parameter used as an operand to the # or ## operators, which is itself subject to further macro replacement, shall only be used as an operand to these operators | Required | 1 | macro parameter used with # and elsewhere |

### Phase 2 - declaration hygiene and dead code

Declaration hygiene, dead code and brace discipline. Mostly mechanical. Rule 15.6 dominates the count and is fixable with `clang-tidy -checks=readability-braces-around-statements -fix`.

15 rules, 4,651 violations to fix.

| Guideline | Category | To fix | Note |
|---|---|---|---|
| Rule 15.6 - The body of an iteration-statement or a selection-statement shall be a compound-statement | Required | 3,755 | braces; clang-tidy readability-braces-around-statements |
| Rule 2.5 - A project should not contain unused macro declarations | Advisory | 241 | unused macro declarations; a cheap cleanup |
| Rule 8.6 - An identifier with external linkage shall have exactly one external definition | Required | 196 | one external definition; an ODR defect class |
| Rule 8.4 - A compatible declaration shall be visible when an object or function with external linkage is defined | Required | 164 | compatible declaration visible |
| Rule 2.7 - There should be no unused parameters in functions | Advisory | 96 | unused parameters |
| Rule 2.3 - A project should not contain unused type declarations | Advisory | 90 | unused type declarations |
| Rule 5.8 - Identifiers that define objects or functions with external linkage shall be unique | Required | 26 | external identifier uniqueness |
| Rule 8.5 - An external object or function shall be declared once in one and only one file | Required | 26 | declared once in one file |
| Rule 14.2 - A for loop shall be well-formed | Required | 25 | for loop well-formed |
| Rule 2.4 - A project should not contain unused tag declarations | Advisory | 11 | unused tag declarations |
| Rule 8.2 - Function types shall be in prototype form with named parameters | Required | 6 | prototype form with named parameters |
| Rule 9.5 - Where designated initializers are used to initialize an array object the size of the array shall be specified explicitly | Required | 6 | designated initializer array size |
| Rule 11.9 - The macro NULL shall be the only permitted form of integer null pointer constant | Required | 5 | NULL is the only null pointer constant |
| Rule 7.2 - A "u" or "U" suffix shall be applied to all integer constants that are represented in an unsigned type | Required | 3 | u suffix on unsigned constants |
| Rule 21.9 - The Standard Library functions bsearch and qsort of <stdlib.h> shall not be used | Required | 1 | bsearch/qsort |

### Phase 3 - the essential type model

The essential type model. Real value, but the largest and least mechanical group. Do it one directory at a time, starting with `src/types`, and only after Phase 1 and Phase 2 are green.

9 rules, 2,151 violations to fix.

| Guideline | Category | To fix | Note |
|---|---|---|---|
| Rule 10.4 - Both operands of an operator in which the usual arithmetic conversions are performed shall have the same essential type category | Required | 1,328 | essential type category; start in src/types |
| Rule 5.6 - A typedef name shall be a unique identifier | Required | 412 | typedef uniqueness; triage first |
| Rule 15.7 - All if ... else if constructs shall be terminated with an else statement | Required | 129 | if...else if needs a terminal else |
| Rule 10.1 - Operands shall not be of an inappropriate essential type | Required | 97 | inappropriate essential type |
| Rule 10.3 - The value of an expression shall not be assigned to an object with a narrower essential type or of a different essential type category | Required | 77 | narrowing assignment |
| Rule 5.7 - A tag name shall be a unique identifier | Required | 64 | tag uniqueness |
| Rule 10.8 - The value of a composite expression shall not be cast to a different essential type category or a wider essential type | Required | 34 | composite expression cast |
| Rule 10.6 - The value of a composite expression shall not be assigned to an object with wider essential type | Required | 9 | composite expression assigned to a wider type |
| Rule 10.7 - If a composite expression is used as one operand of an operator in which the usual arithmetic conversions are performed then the other operand shall not have wider essential type | Required | 1 | composite operand of a wider type |

## Every guideline

| Guideline | Category | Decision | Violations | Checked by cppcheck |
|---|---|---|---|---|
| Dir 1.1 - Any implementation-defined behaviour on which the output of the program depends shall be documented and understood | Required | Adopted, enforced by other means | - | no |
| Dir 2.1 - All source files shall compile without any compilation errors | Required | Adopted, clean | - | no |
| Dir 3.1 - All code shall be traceable to documented requirements | Required | Adopted, enforced by other means | - | no |
| Dir 4.1 - Run-time failures shall be minimized | Required | Adopted, enforced by other means | - | no |
| Dir 4.2 - All usage of assembly language should be documented | Advisory | Adopted, clean | - | no |
| Dir 4.3 - Assembly language shall be encapsulated and isolated | Required | Adopted, clean | - | no |
| Dir 4.4 - Sections of code should not be "commented out" | Advisory | Adopted, enforced by other means | - | no |
| Dir 4.5 - Identifiers in the same name space with overlapping visibility should be typographically unambiguous | Advisory | Adopted, enforced by other means | - | no |
| Dir 4.6 - typedefs that indicate size and signedness should be used in place of the basic numerical types | Advisory | Declined (advisory) | - | no |
| Dir 4.7 - If a function returns error information, then that error information shall be tested | Required | Adopted, enforced by other means | - | no |
| Dir 4.8 - If a pointer to a structure or union is never dereferenced within a translation unit, then the implementation of the object should be hidden | Advisory | Declined (advisory) | - | no |
| Dir 4.9 - A function should be used in preference to a function-like macro where they are interchangeable | Advisory | Declined (advisory) | - | no |
| Dir 4.10 - Precautions shall be taken in order to prevent the contents of a header file being included more than once | Required | Adopted, clean | - | no |
| Dir 4.11 - The validity of values passed to library functions shall be checked | Required | Adopted, enforced by other means | - | no |
| Dir 4.12 - Dynamic memory allocation shall not be used | Required | Deviation record (D-11) | - | no |
| Dir 4.13 - Functions which are designed to provide operations on a resource should be called in an appropriate sequence | Advisory | Adopted, enforced by other means | - | no |
| Dir 4.14 - The validity of values received from external sources shall be checked | Required | Adopted, enforced by other means | - | no |
| Rule 1.1 - The program shall contain no violations of the standard C syntax and constraints, and shall not exceed the implementation's translation limits | Required | Adopted, enforced by other means | - | no |
| Rule 1.2 - Language extensions should not be used | Advisory | Adopted, clean | - | yes |
| Rule 1.3 - There shall be no occurrence of undefined or critical unspecified behaviour | Required | Adopted, enforced by other means | - | no |
| Rule 1.4 - Not present in MISRA C:2012 Third edition first revision - added by a later amendment for C11/C18. Text unavailable from this PDF. | Required | Outside this edition | 4 | yes |
| Rule 2.1 - A project shall not contain unreachable code | Required | Adopted, enforced by other means | - | no |
| Rule 2.2 - There shall be no dead code | Required | Adopted, fix backlog (P1) | 3 | yes |
| Rule 2.3 - A project should not contain unused type declarations | Advisory | Adopted, fix backlog (P2) | 90 | yes |
| Rule 2.4 - A project should not contain unused tag declarations | Advisory | Adopted, fix backlog (P2) | 11 | yes |
| Rule 2.5 - A project should not contain unused macro declarations | Advisory | Adopted, fix backlog (P2) | 241 | yes |
| Rule 2.6 - A function should not contain unused label declarations | Advisory | Adopted, enforced by other means | - | no |
| Rule 2.7 - There should be no unused parameters in functions | Advisory | Adopted, fix backlog (P2) | 96 | yes |
| Rule 3.1 - The character sequences /* and // shall not be used within a comment | Required | Adopted, fix backlog (P1) | 6 | yes |
| Rule 3.2 - Line-splicing shall not be used in // comments | Required | Adopted, enforced by other means | - | no |
| Rule 4.1 - Octal and hexadecimal escape sequences shall be terminated | Required | Adopted, fix backlog (P1) | 41 | yes |
| Rule 4.2 - Trigraphs should not be used | Advisory | Adopted, clean | - | yes |
| Rule 5.1 - External identifiers shall be distinct | Required | Adopted, clean | - | yes |
| Rule 5.2 - Identifiers declared in the same scope and name space shall be distinct | Required | Adopted, clean | - | yes |
| Rule 5.3 - An identifier declared in an inner scope shall not hide an identifier declared in an outer scope | Required | Adopted, enforced by other means | - | no |
| Rule 5.4 - Macro identifiers shall be distinct | Required | Adopted, clean | - | yes |
| Rule 5.5 - Identifiers shall be distinct from macro names | Required | Adopted, clean | - | yes |
| Rule 5.6 - A typedef name shall be a unique identifier | Required | Adopted, fix backlog (P3) | 412 | yes |
| Rule 5.7 - A tag name shall be a unique identifier | Required | Adopted, fix backlog (P3) | 64 | yes |
| Rule 5.8 - Identifiers that define objects or functions with external linkage shall be unique | Required | Adopted, fix backlog (P2) | 26 | yes |
| Rule 5.9 - Identifiers that define objects or functions with internal linkage should be unique | Advisory | Declined (advisory) | 75 | yes |
| Rule 6.1 - Bit-fields shall only be declared with an appropriate type | Required | Adopted, clean | - | yes |
| Rule 6.2 - Single-bit named bit fields shall not be of a signed type | Required | Adopted, clean | - | yes |
| Rule 7.1 - Octal constants shall not be used | Required | Adopted, fix backlog (P1) | 4 | yes |
| Rule 7.2 - A "u" or "U" suffix shall be applied to all integer constants that are represented in an unsigned type | Required | Adopted, fix backlog (P2) | 3 | yes |
| Rule 7.3 - The lowercase character "l" shall not be used in a literal suffix | Required | Adopted, clean | 384 | yes |
| Rule 7.4 - A string literal shall not be assigned to an object unless the object's type is "pointer to const-qualified char" | Required | Adopted, fix backlog (P1) | 11 | yes |
| Rule 8.1 - Types shall be explicitly specified | Required | Adopted, clean | - | yes |
| Rule 8.2 - Function types shall be in prototype form with named parameters | Required | Adopted, fix backlog (P2) | 6 | yes |
| Rule 8.3 - All declarations of an object or function shall use the same names and type qualifiers | Required | Adopted, enforced by other means | - | no |
| Rule 8.4 - A compatible declaration shall be visible when an object or function with external linkage is defined | Required | Adopted, fix backlog (P2) | 164 | yes |
| Rule 8.5 - An external object or function shall be declared once in one and only one file | Required | Adopted, fix backlog (P2) | 26 | yes |
| Rule 8.6 - An identifier with external linkage shall have exactly one external definition | Required | Adopted, fix backlog (P2) | 196 | yes |
| Rule 8.7 - Functions and objects should not be defined with external linkage if they are referenced in only one translation unit | Advisory | Declined (advisory) | 1197 | yes |
| Rule 8.8 - The static storage class specifier shall be used in all declarations of objects and functions that have internal linkage | Required | Adopted, clean | - | yes |
| Rule 8.9 - An object should be defined at block scope if its identifier only appears in a single function | Advisory | Declined (advisory) | 24 | yes |
| Rule 8.10 - An inline function shall be declared with the static storage class | Required | Adopted, clean | - | yes |
| Rule 8.11 - When an array with external linkage is declared, its size should be explicitly specified | Advisory | Adopted, clean | - | yes |
| Rule 8.12 - Within an enumerator list, the value of an implicitly-specified enumeration constant shall be unique | Required | Adopted, clean | - | yes |
| Rule 8.13 - A pointer should point to a const-qualified type whenever possible | Advisory | Adopted, enforced by other means | - | no |
| Rule 8.14 - The restrict type qualifier shall not be used | Required | Adopted, clean | - | yes |
| Rule 9.1 - The value of an object with automatic storage duration shall not be read before it has been set | Mandatory | Adopted, enforced by other means | - | no |
| Rule 9.2 - The initializer for an aggregate or union shall be enclosed in braces | Required | Adopted, clean | - | yes |
| Rule 9.3 - Arrays shall not be partially initialized | Required | Adopted, fix backlog (P1) | 1 | yes |
| Rule 9.4 - An element of an object shall not be initialized more than once | Required | Adopted, clean | - | yes |
| Rule 9.5 - Where designated initializers are used to initialize an array object the size of the array shall be specified explicitly | Required | Adopted, fix backlog (P2) | 6 | yes |
| Rule 10.1 - Operands shall not be of an inappropriate essential type | Required | Adopted, fix backlog (P3) | 97 | yes |
| Rule 10.2 - Expressions of essentially character type shall not be used inappropriately in addition and subtraction operations | Required | Adopted, clean | - | yes |
| Rule 10.3 - The value of an expression shall not be assigned to an object with a narrower essential type or of a different essential type category | Required | Adopted, fix backlog (P3) | 77 | yes |
| Rule 10.4 - Both operands of an operator in which the usual arithmetic conversions are performed shall have the same essential type category | Required | Adopted, fix backlog (P3) | 1328 | yes |
| Rule 10.5 - The value of an expression should not be cast to an inappropriate essential type | Advisory | Adopted, clean | - | yes |
| Rule 10.6 - The value of a composite expression shall not be assigned to an object with wider essential type | Required | Adopted, fix backlog (P3) | 9 | yes |
| Rule 10.7 - If a composite expression is used as one operand of an operator in which the usual arithmetic conversions are performed then the other operand shall not have wider essential type | Required | Adopted, fix backlog (P3) | 1 | yes |
| Rule 10.8 - The value of a composite expression shall not be cast to a different essential type category or a wider essential type | Required | Adopted, fix backlog (P3) | 34 | yes |
| Rule 11.1 - Conversions shall not be performed between a pointer to a function and any other type | Required | Adopted, fix backlog (P1) | 2 | yes |
| Rule 11.2 - Conversions shall not be performed between a pointer to an incomplete type and any other type | Required | Adopted, clean | - | yes |
| Rule 11.3 - A cast shall not be performed between a pointer to object type and a pointer to a different object type | Required | Deviation record (D-05) | 912 | yes |
| Rule 11.4 - A conversion should not be performed between a pointer to object and an integer type | Advisory | Declined (advisory) | 4 | yes |
| Rule 11.5 - A conversion should not be performed from pointer to void into pointer to object | Advisory | Declined (advisory) | 1216 | yes |
| Rule 11.6 - A cast shall not be performed between pointer to void and an arithmetic type | Required | Adopted, fix backlog (P1) | 12 | yes |
| Rule 11.7 - A cast shall not be performed between pointer to object and a non- integer arithmetic type | Required | Adopted, clean | - | yes |
| Rule 11.8 - A cast shall not remove any const or volatile qualification from the type pointed to by a pointer | Required | Adopted, fix backlog (P1) | 67 | yes |
| Rule 11.9 - The macro NULL shall be the only permitted form of integer null pointer constant | Required | Adopted, fix backlog (P2) | 5 | yes |
| Rule 12.1 - The precedence of operators within expressions should be made explicit | Advisory | Declined (advisory) | 1463 | yes |
| Rule 12.2 - The right hand operand of a shift operator shall lie in the range zero to one less than the width in bits of the essential type of the left hand operand | Required | Adopted, fix backlog (P1) | 45 | yes |
| Rule 12.3 - The comma operator should not be used | Advisory | Declined (advisory) | 41 | yes |
| Rule 12.4 - Evaluation of constant expressions should not lead to unsigned integer wrap-around | Advisory | Adopted, clean | - | yes |
| Rule 12.5 - The sizeof operator shall not have an operand which is a function parameter declared as "array of type" | Mandatory | Adopted, enforced by other means | - | no |
| Rule 13.1 - Initializer lists shall not contain persistent side effects | Required | Adopted, clean | - | yes |
| Rule 13.2 - The value of an expression and its persistent side effects shall be the same under all permitted evaluation orders | Required | Adopted, enforced by other means | - | no |
| Rule 13.3 - A full expression containing an increment (++) or decrement (--) operator should have no other potential side effects other than that caused by the increment or decrement operator | Advisory | Declined (advisory) | 148 | yes |
| Rule 13.4 - The result of an assignment operator should not be used | Advisory | Adopted, fix backlog (P1) | 14 | yes |
| Rule 13.5 - The right hand operand of a logical && or || operator shall not contain persistent side effects | Required | Adopted, clean | - | yes |
| Rule 13.6 - The operand of the sizeof operator shall not contain any expression which has potential side effects | Mandatory | Adopted, clean | - | yes |
| Rule 14.1 - A loop counter shall not have essentially floating type | Required | Adopted, clean | - | yes |
| Rule 14.2 - A for loop shall be well-formed | Required | Adopted, fix backlog (P2) | 25 | yes |
| Rule 14.3 - Controlling expressions shall not be invariant | Required | Adopted, enforced by other means | - | no |
| Rule 14.4 - The controlling expression of an if statement and the controlling expression of an iteration-statement shall have essentially Boolean type | Required | Deviation record (D-03) | 2111 | yes |
| Rule 15.1 - The goto statement should not be used | Advisory | Adopted, fix backlog (P1) | 1 | yes |
| Rule 15.2 - The goto statement shall jump to a label declared later in the same function | Required | Adopted, clean | - | yes |
| Rule 15.3 - Any label referenced by a goto statement shall be declared in the same block, or in any block enclosing the goto statement | Required | Adopted, fix backlog (P1) | 1 | yes |
| Rule 15.4 - There should be no more than one break or goto statement used to terminate any iteration statement | Advisory | Declined (advisory) | 7 | yes |
| Rule 15.5 - A function should have a single point of exit at the end | Advisory | Declined (advisory) | 6858 | yes |
| Rule 15.6 - The body of an iteration-statement or a selection-statement shall be a compound-statement | Required | Adopted, fix backlog (P2) | 3755 | yes |
| Rule 15.7 - All if ... else if constructs shall be terminated with an else statement | Required | Adopted, fix backlog (P3) | 129 | yes |
| Rule 16.1 - All switch statements shall be well-formed | Required | Adopted, fix backlog (P1) | 1 | yes |
| Rule 16.2 - A switch label shall only be used when the most closely-enclosing compound statement is the body of a switch statement | Required | Adopted, fix backlog (P1) | 5 | yes |
| Rule 16.3 - An unconditional break statement shall terminate every switch-clause | Required | Adopted, fix backlog (P1) | 12 | yes |
| Rule 16.4 - Every switch statement shall have a default label | Required | Adopted, fix backlog (P1) | 26 | yes |
| Rule 16.5 - A default label shall appear as either the first or the last switch label of a switch statement | Required | Adopted, clean | - | yes |
| Rule 16.6 - Every switch statement shall have at least two switch-clauses | Required | Adopted, clean | - | yes |
| Rule 16.7 - A switch-expression shall not have essentially Boolean type | Required | Adopted, clean | - | yes |
| Rule 17.1 - The features of <stdarg.h> shall not be used | Required | Deviation record (D-07) | 50 | yes |
| Rule 17.2 - Functions shall not call themselves, either directly or indirectly | Required | Deviation record (D-06) | 805 | yes |
| Rule 17.3 - A function shall not be declared implicitly | Mandatory | Adopted, clean | 660 | yes |
| Rule 17.4 - All exit paths from a function with non-void return type shall have an explicit return statement with an expression | Mandatory | Adopted, enforced by other means | - | no |
| Rule 17.5 - The function argument corresponding to a parameter declared to have an array type shall have an appropriate number of elements | Advisory | Adopted, enforced by other means | - | no |
| Rule 17.6 - The declaration of an array parameter shall not contain the static keyword between the [ ] | Mandatory | Adopted, clean | - | yes |
| Rule 17.7 - The value returned by a function having non-void return type shall be used | Required | Deviation record (D-02) | 3225 | yes |
| Rule 17.8 - A function parameter should not be modified | Advisory | Declined (advisory) | 53 | yes |
| Rule 18.1 - A pointer resulting from arithmetic on a pointer operand shall address an element of the same array as that pointer operand | Required | Adopted, enforced by other means | - | no |
| Rule 18.2 - Subtraction between pointers shall only be applied to pointers that address elements of the same array | Required | Adopted, enforced by other means | - | no |
| Rule 18.3 - The relational operators >, >=, < and <= shall not be applied to objects of pointer type except where they point into the same object | Required | Adopted, enforced by other means | - | no |
| Rule 18.4 - The +, -, += and -= operators should not be applied to an expression of pointer type | Advisory | Declined (advisory) | 170 | yes |
| Rule 18.5 - Declarations should contain no more than two levels of pointer nesting | Advisory | Declined (advisory) | 14 | yes |
| Rule 18.6 - The address of an object with automatic storage shall not be copied to another object that persists after the first object has ceased to exist | Required | Adopted, enforced by other means | - | no |
| Rule 18.7 - Flexible array members shall not be declared | Required | Adopted, clean | - | yes |
| Rule 18.8 - Variable-length array types shall not be used | Required | Adopted, clean | - | yes |
| Rule 19.1 - An object shall not be assigned or copied to an overlapping object | Mandatory | Adopted, enforced by other means | - | no |
| Rule 19.2 - The union keyword should not be used | Advisory | Declined (advisory) | 55 | yes |
| Rule 20.1 - #include directives should only be preceded by preprocessor directives or comments | Advisory | Declined (advisory) | 6 | yes |
| Rule 20.2 - The ', " or \ characters and the /* or // character sequences shall not occur in a header file name | Required | Adopted, clean | - | yes |
| Rule 20.3 - The #include directive shall be followed by either a <filename> or "filename" sequence | Required | Adopted, clean | - | yes |
| Rule 20.4 - A macro shall not be defined with the same name as a keyword | Required | Adopted, clean | - | yes |
| Rule 20.5 - #undef should not be used | Advisory | Declined (advisory) | 2 | yes |
| Rule 20.6 - Tokens that look like a preprocessing directive shall not occur within a macro argument | Required | Adopted, enforced by other means | - | no |
| Rule 20.7 - Expressions resulting from the expansion of macro parameters shall be enclosed in parentheses | Required | Adopted, fix backlog (P1) | 39 | yes |
| Rule 20.8 - The controlling expression of a #if or #elif preprocessing directive shall evaluate to 0 or 1 | Required | Adopted, clean | - | yes |
| Rule 20.9 - All identifiers used in the controlling expression of #if or #elif preprocessing directives shall be #define'd before evaluation | Required | Adopted, clean | - | yes |
| Rule 20.10 - The # and ## preprocessor operators should not be used | Advisory | Declined (advisory) | 44 | yes |
| Rule 20.11 - A macro parameter immediately following a # operator shall not immediately be followed by a ## operator | Required | Adopted, clean | - | yes |
| Rule 20.12 - A macro parameter used as an operand to the # or ## operators, which is itself subject to further macro replacement, shall only be used as an operand to these operators | Required | Adopted, fix backlog (P1) | 1 | yes |
| Rule 20.13 - A line whose first token is # shall be a valid preprocessing directive | Required | Adopted, clean | - | yes |
| Rule 20.14 - All #else, #elif and #endif preprocessor directives shall reside in the same file as the #if, #ifdef or #ifndef directive to which they are related | Required | Adopted, clean | - | yes |
| Rule 21.1 - #define and #undef shall not be used on a reserved identifier or reserved macro name | Required | Adopted, fix backlog (P1) | 9 | yes |
| Rule 21.2 - A reserved identifier or reserved macro name shall not be declared | Required | Adopted, clean | - | yes |
| Rule 21.3 - The memory allocation and deallocation functions of <stdlib.h> shall not be used | Required | Deviation record (D-01) | 2597 | yes |
| Rule 21.4 - The standard header file <setjmp.h> shall not be used | Required | Deviation record (D-10) | 3 | yes |
| Rule 21.5 - The standard header file <signal.h> shall not be used | Required | Adopted, clean | - | yes |
| Rule 21.6 - The Standard Library input/output functions shall not be used | Required | Deviation record (D-04) | 1372 | yes |
| Rule 21.7 - The Standard Library functions atof, atoi, atol and atoll functions of <stdlib.h> shall not be used | Required | Adopted, fix backlog (P1) | 11 | yes |
| Rule 21.8 - The Standard Library functions abort, exit, getenv and system of <stdlib.h> shall not be used | Required | Deviation record (D-09) | 14 | yes |
| Rule 21.9 - The Standard Library functions bsearch and qsort of <stdlib.h> shall not be used | Required | Adopted, fix backlog (P2) | 1 | yes |
| Rule 21.10 - The Standard Library time and date functions shall not be used | Required | Deviation record (D-08) | 47 | yes |
| Rule 21.11 - The standard header file <tgmath.h> shall not be used | Required | Adopted, clean | - | yes |
| Rule 21.12 - The exception handling features of <fenv.h> should not be used | Advisory | Adopted, clean | - | yes |
| Rule 21.13 - Any value passed to a function in <ctype.h> shall be representable as an unsigned char or be the value EOF | Mandatory | Adopted, enforced by other means | - | no |
| Rule 21.14 - The Standard Library function memcmp shall not be used to compare null terminated strings | Required | Adopted, clean | - | yes |
| Rule 21.15 - The pointer arguments to the Standard Library functions memcpy, memmove and memcmp shall be pointers to qualified or unqualified versions of compatible types | Required | Adopted, fix backlog (P1) | 2 | yes |
| Rule 21.16 - The pointer arguments to the Standard Library function memcmp shall point to either a pointer type, an essentially signed type, an essentially unsigned type, an essentially Boolean type or an essentially enum type | Required | Adopted, fix backlog (P1) | 5 | yes |
| Rule 21.17 - Use of the string handling functions from <string.h> shall not result in accesses beyond the bounds of the objects referenced by their pointer parameters | Mandatory | Adopted, enforced by other means | - | no |
| Rule 21.18 - The size_t argument passed to any function in <string.h> shall have an appropriate value | Mandatory | Adopted, enforced by other means | - | no |
| Rule 21.19 - The pointers returned by the Standard Library functions localeconv, getenv, setlocale or, strerror shall only be used as if they have pointer to const-qualified type | Mandatory | Adopted, clean | - | yes |
| Rule 21.20 - The pointer returned by the Standard Library functions asctime, ctime, gmtime, localtime, localeconv, getenv, setlocale or strerror shall not be used following a subsequent call to the same function | Mandatory | Adopted, clean | - | yes |
| Rule 21.21 - Not present in MISRA C:2012 Third edition first revision - added by a later amendment. Text unavailable from this PDF. | Required | Outside this edition | 3 | yes |
| Rule 22.1 - All resources obtained dynamically by means of Standard Library functions shall be explicitly released | Required | Adopted, enforced by other means | - | no |
| Rule 22.2 - A block of memory shall only be freed if it was allocated by means of a Standard Library function | Mandatory | Adopted, enforced by other means | - | no |
| Rule 22.3 - The same file shall not be open for read and write access at the same time on different streams | Required | Adopted, enforced by other means | - | no |
| Rule 22.4 - There shall be no attempt to write to a stream which has been opened as read-only | Mandatory | Adopted, enforced by other means | - | no |
| Rule 22.5 - A pointer to a FILE object shall not be dereferenced | Mandatory | Adopted, clean | - | yes |
| Rule 22.6 - The value of a pointer to a FILE shall not be used after the associated stream has been closed | Mandatory | Adopted, enforced by other means | - | no |
| Rule 22.7 - The macro EOF shall only be compared with the unmodified return value from any Standard Library function capable of returning EOF | Required | Adopted, clean | - | yes |
| Rule 22.8 - The value of errno shall be set to zero prior to a call to an errno-setting- function | Required | Adopted, fix backlog (P1) | 18 | yes |
| Rule 22.9 - The value of errno shall be tested against zero after calling an errno- setting-function | Required | Adopted, fix backlog (P1) | 18 | yes |
| Rule 22.10 - The value of errno shall only be tested when the last function to be called was an errno-setting-function | Required | Adopted, fix backlog (P1) | 2 | yes |

## Running the gate

    make misra                     # fail on any violation not in the baseline
    make misra-baseline            # accept the current findings as the baseline
    scripts/misra-scan.sh --report # full breakdown, never fails

`make misra` does not fail on the 7,168 accepted violations. It records the
count per file and rule in `scripts/misra-baseline.txt` and fails only when a
count rises or a new pair appears. This is the same shape as `make safety`.

Setup is one step and it is not automatic: the cppcheck addon needs a rule-text
file built from **your own licensed copy** of the standard. That file is not in
this repository, because the guideline text is copyrighted. See
`tools/README.md`. Until it exists, `make misra` exits 2 with an explanation
rather than passing silently.

For the same reason the gate is **not** part of `make verify-core`. A fresh
checkout cannot run it, so wiring it into the shared gate would break the build
for anyone without the PDF.

Verified 2026-08-14 that the gate has teeth: adding an octal constant in a new
file made it report `rule 7.1  0 -> 1` and exit 1; removing the file returned it
to exit 0. A gate that has never been seen to fail is not a gate.

After a fix, tighten the baseline. `make misra` prints how many violations
fell, and a baseline that is never tightened stops being a gate.

See `enforcement-plan.md` for what each adopted guideline is enforced by.

## Known limits of the checker

Read these before trusting any number this tool produces.

- **cppcheck exits 0 with violations present.** A gate must pass
  `--error-exitcode=1`, or it will report green forever.
- **Rule 7.3 is broken in the addon.** Its regex
  `^(0[xX])?[0-9a-fA-FpP.]+[Uu]*l+[Uu]*$` matches the identifiers `call`,
  `decl`, `al` and `bl` as if they were numeric literals. In the baseline
  scan, 382 of 384 reports were identifiers. The rule stays adopted, because
  the two real reports are real; the noise must be suppressed per site.
- **Rule 17.3 reports library calls as implicit declarations.** cppcheck has
  no signature for `atomic_*`, `pthread_cond_*`, `curl_*` or `json_object_*`,
  so it reports every call as an implicit declaration. C23 makes a true
  implicit declaration a constraint violation, so a build that passes
  `-std=c23 -Wall -Wextra` cannot contain one. Adding `--library=posix
  --library=gnu` cut this from 1,666 to 660.
- **Generated code is excluded.** `src/parser/parser.tab.c` and the `parser.y`
  lines its `#line` directives point at produce 1,542 findings, 912 of them
  Rule 20.13. MISRA section 6.8 treats generated code separately.
- **cppcheck stops at C11.** Any C23-only construct is parsed as C11.
- **Platform-conditional code is invisible.** The scan runs on Linux, so an
  `#ifdef _WIN32` branch is never preprocessed and never checked. The `system()`
  call at `src/codegen/codegen.c:1797` is a real Rule 21.8 violation that the
  baseline reports as absent. A guideline reading clean means clean *on this
  platform's preprocessed source*, not clean in the tree.
