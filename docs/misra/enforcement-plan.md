# MISRA guideline enforcement plan

For each adopted guideline, what actually enforces it. MISRA 5.5 calls this a
compliance matrix. A guideline with no enforcement listed is not adopted; see
`README.md`.

Enforcement mechanisms in this project:

| Tag | Mechanism |
|---|---|
| `cppcheck` | The cppcheck MISRA addon, run over `src/` |
| `compiler` | `gcc -Wall -Wextra -std=c23`, which `make verify-core` requires to be clean |
| `valgrind` | `arena-valgrind-probe`, `ast-free-leak-probe` and the other valgrind gates |
| `sanitizer` | `far-transport-asan` (ASan) and the TSan targets |
| `fuzzer` | `bin/fuzz_parse`, see `tests/fuzz/README.md` |
| `review` | Code review, with no tool support |

| Guideline | Category | Decision | Enforced by |
|---|---|---|---|
| Dir 1.1 - Any implementation-defined behaviour on which the output of the program depends shall be documented and understood | Required | Adopted, enforced by other means | review |
| Dir 2.1 - All source files shall compile without any compilation errors | Required | Adopted, clean | compiler |
| Dir 3.1 - All code shall be traceable to documented requirements | Required | Adopted, enforced by other means | review |
| Dir 4.1 - Run-time failures shall be minimized | Required | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Dir 4.2 - All usage of assembly language should be documented | Advisory | Adopted, clean | review (1 site) |
| Dir 4.3 - Assembly language shall be encapsulated and isolated | Required | Adopted, clean | review (1 site) |
| Dir 4.4 - Sections of code should not be "commented out" | Advisory | Adopted, enforced by other means | review |
| Dir 4.5 - Identifiers in the same name space with overlapping visibility should be typographically unambiguous | Advisory | Adopted, enforced by other means | review |
| Dir 4.7 - If a function returns error information, then that error information shall be tested | Required | Adopted, enforced by other means | review |
| Dir 4.10 - Precautions shall be taken in order to prevent the contents of a header file being included more than once | Required | Adopted, clean | compiler |
| Dir 4.11 - The validity of values passed to library functions shall be checked | Required | Adopted, enforced by other means | review, fuzzer |
| Dir 4.13 - Functions which are designed to provide operations on a resource should be called in an appropriate sequence | Advisory | Adopted, enforced by other means | review |
| Dir 4.14 - The validity of values received from external sources shall be checked | Required | Adopted, enforced by other means | fuzzer |
| Rule 1.1 - The program shall contain no violations of the standard C syntax and constraints, and shall not exceed the implementation's translation limits | Required | Adopted, enforced by other means | compiler |
| Rule 1.2 - Language extensions should not be used | Advisory | Adopted, clean | cppcheck |
| Rule 1.3 - There shall be no occurrence of undefined or critical unspecified behaviour | Required | Adopted, enforced by other means | compiler |
| Rule 2.1 - A project shall not contain unreachable code | Required | Adopted, enforced by other means | compiler |
| Rule 2.2 - There shall be no dead code | Required | Adopted, fix backlog | cppcheck |
| Rule 2.3 - A project should not contain unused type declarations | Advisory | Adopted, fix backlog | cppcheck |
| Rule 2.4 - A project should not contain unused tag declarations | Advisory | Adopted, fix backlog | cppcheck |
| Rule 2.5 - A project should not contain unused macro declarations | Advisory | Adopted, fix backlog | cppcheck |
| Rule 2.6 - A function should not contain unused label declarations | Advisory | Adopted, enforced by other means | review |
| Rule 2.7 - There should be no unused parameters in functions | Advisory | Adopted, fix backlog | cppcheck |
| Rule 3.1 - The character sequences /* and // shall not be used within a comment | Required | Adopted, fix backlog | cppcheck |
| Rule 3.2 - Line-splicing shall not be used in // comments | Required | Adopted, enforced by other means | compiler |
| Rule 4.1 - Octal and hexadecimal escape sequences shall be terminated | Required | Adopted, fix backlog | cppcheck |
| Rule 4.2 - Trigraphs should not be used | Advisory | Adopted, clean | cppcheck |
| Rule 5.1 - External identifiers shall be distinct | Required | Adopted, clean | cppcheck |
| Rule 5.2 - Identifiers declared in the same scope and name space shall be distinct | Required | Adopted, clean | cppcheck |
| Rule 5.3 - An identifier declared in an inner scope shall not hide an identifier declared in an outer scope | Required | Adopted, enforced by other means | compiler |
| Rule 5.4 - Macro identifiers shall be distinct | Required | Adopted, clean | cppcheck |
| Rule 5.5 - Identifiers shall be distinct from macro names | Required | Adopted, clean | cppcheck |
| Rule 5.6 - A typedef name shall be a unique identifier | Required | Adopted, fix backlog | cppcheck |
| Rule 5.7 - A tag name shall be a unique identifier | Required | Adopted, fix backlog | cppcheck |
| Rule 5.8 - Identifiers that define objects or functions with external linkage shall be unique | Required | Adopted, fix backlog | cppcheck |
| Rule 6.1 - Bit-fields shall only be declared with an appropriate type | Required | Adopted, clean | cppcheck |
| Rule 6.2 - Single-bit named bit fields shall not be of a signed type | Required | Adopted, clean | cppcheck |
| Rule 7.1 - Octal constants shall not be used | Required | Adopted, fix backlog | cppcheck |
| Rule 7.2 - A "u" or "U" suffix shall be applied to all integer constants that are represented in an unsigned type | Required | Adopted, fix backlog | cppcheck |
| Rule 7.3 - The lowercase character "l" shall not be used in a literal suffix | Required | Adopted, clean | cppcheck |
| Rule 7.4 - A string literal shall not be assigned to an object unless the object's type is "pointer to const-qualified char" | Required | Adopted, fix backlog | cppcheck |
| Rule 8.1 - Types shall be explicitly specified | Required | Adopted, clean | cppcheck |
| Rule 8.2 - Function types shall be in prototype form with named parameters | Required | Adopted, fix backlog | cppcheck |
| Rule 8.3 - All declarations of an object or function shall use the same names and type qualifiers | Required | Adopted, enforced by other means | compiler |
| Rule 8.4 - A compatible declaration shall be visible when an object or function with external linkage is defined | Required | Adopted, fix backlog | cppcheck |
| Rule 8.5 - An external object or function shall be declared once in one and only one file | Required | Adopted, fix backlog | cppcheck |
| Rule 8.6 - An identifier with external linkage shall have exactly one external definition | Required | Adopted, fix backlog | cppcheck |
| Rule 8.8 - The static storage class specifier shall be used in all declarations of objects and functions that have internal linkage | Required | Adopted, clean | cppcheck |
| Rule 8.10 - An inline function shall be declared with the static storage class | Required | Adopted, clean | cppcheck |
| Rule 8.11 - When an array with external linkage is declared, its size should be explicitly specified | Advisory | Adopted, clean | cppcheck |
| Rule 8.12 - Within an enumerator list, the value of an implicitly-specified enumeration constant shall be unique | Required | Adopted, clean | cppcheck |
| Rule 8.13 - A pointer should point to a const-qualified type whenever possible | Advisory | Adopted, enforced by other means | review |
| Rule 8.14 - The restrict type qualifier shall not be used | Required | Adopted, clean | cppcheck |
| Rule 9.1 - The value of an object with automatic storage duration shall not be read before it has been set | Mandatory | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 9.2 - The initializer for an aggregate or union shall be enclosed in braces | Required | Adopted, clean | cppcheck |
| Rule 9.3 - Arrays shall not be partially initialized | Required | Adopted, fix backlog | cppcheck |
| Rule 9.4 - An element of an object shall not be initialized more than once | Required | Adopted, clean | cppcheck |
| Rule 9.5 - Where designated initializers are used to initialize an array object the size of the array shall be specified explicitly | Required | Adopted, fix backlog | cppcheck |
| Rule 10.1 - Operands shall not be of an inappropriate essential type | Required | Adopted, fix backlog | cppcheck |
| Rule 10.2 - Expressions of essentially character type shall not be used inappropriately in addition and subtraction operations | Required | Adopted, clean | cppcheck |
| Rule 10.3 - The value of an expression shall not be assigned to an object with a narrower essential type or of a different essential type category | Required | Adopted, fix backlog | cppcheck |
| Rule 10.4 - Both operands of an operator in which the usual arithmetic conversions are performed shall have the same essential type category | Required | Adopted, fix backlog | cppcheck |
| Rule 10.5 - The value of an expression should not be cast to an inappropriate essential type | Advisory | Adopted, clean | cppcheck |
| Rule 10.6 - The value of a composite expression shall not be assigned to an object with wider essential type | Required | Adopted, fix backlog | cppcheck |
| Rule 10.7 - If a composite expression is used as one operand of an operator in which the usual arithmetic conversions are performed then the other operand shall not have wider essential type | Required | Adopted, fix backlog | cppcheck |
| Rule 10.8 - The value of a composite expression shall not be cast to a different essential type category or a wider essential type | Required | Adopted, fix backlog | cppcheck |
| Rule 11.1 - Conversions shall not be performed between a pointer to a function and any other type | Required | Adopted, fix backlog | cppcheck |
| Rule 11.2 - Conversions shall not be performed between a pointer to an incomplete type and any other type | Required | Adopted, clean | cppcheck |
| Rule 11.6 - A cast shall not be performed between pointer to void and an arithmetic type | Required | Adopted, fix backlog | cppcheck |
| Rule 11.7 - A cast shall not be performed between pointer to object and a non- integer arithmetic type | Required | Adopted, clean | cppcheck |
| Rule 11.8 - A cast shall not remove any const or volatile qualification from the type pointed to by a pointer | Required | Adopted, fix backlog | cppcheck |
| Rule 11.9 - The macro NULL shall be the only permitted form of integer null pointer constant | Required | Adopted, fix backlog | cppcheck |
| Rule 12.2 - The right hand operand of a shift operator shall lie in the range zero to one less than the width in bits of the essential type of the left hand operand | Required | Adopted, fix backlog | cppcheck |
| Rule 12.4 - Evaluation of constant expressions should not lead to unsigned integer wrap-around | Advisory | Adopted, clean | cppcheck |
| Rule 12.5 - The sizeof operator shall not have an operand which is a function parameter declared as "array of type" | Mandatory | Adopted, enforced by other means | review |
| Rule 13.1 - Initializer lists shall not contain persistent side effects | Required | Adopted, clean | cppcheck |
| Rule 13.2 - The value of an expression and its persistent side effects shall be the same under all permitted evaluation orders | Required | Adopted, enforced by other means | review |
| Rule 13.4 - The result of an assignment operator should not be used | Advisory | Adopted, fix backlog | cppcheck |
| Rule 13.5 - The right hand operand of a logical && or || operator shall not contain persistent side effects | Required | Adopted, clean | cppcheck |
| Rule 13.6 - The operand of the sizeof operator shall not contain any expression which has potential side effects | Mandatory | Adopted, clean | cppcheck |
| Rule 14.1 - A loop counter shall not have essentially floating type | Required | Adopted, clean | cppcheck |
| Rule 14.2 - A for loop shall be well-formed | Required | Adopted, fix backlog | cppcheck |
| Rule 14.3 - Controlling expressions shall not be invariant | Required | Adopted, enforced by other means | review |
| Rule 15.1 - The goto statement should not be used | Advisory | Adopted, fix backlog | cppcheck |
| Rule 15.2 - The goto statement shall jump to a label declared later in the same function | Required | Adopted, clean | cppcheck |
| Rule 15.3 - Any label referenced by a goto statement shall be declared in the same block, or in any block enclosing the goto statement | Required | Adopted, fix backlog | cppcheck |
| Rule 15.6 - The body of an iteration-statement or a selection-statement shall be a compound-statement | Required | Adopted, fix backlog | cppcheck |
| Rule 15.7 - All if ... else if constructs shall be terminated with an else statement | Required | Adopted, fix backlog | cppcheck |
| Rule 16.1 - All switch statements shall be well-formed | Required | Adopted, fix backlog | cppcheck |
| Rule 16.2 - A switch label shall only be used when the most closely-enclosing compound statement is the body of a switch statement | Required | Adopted, fix backlog | cppcheck |
| Rule 16.3 - An unconditional break statement shall terminate every switch-clause | Required | Adopted, fix backlog | cppcheck |
| Rule 16.4 - Every switch statement shall have a default label | Required | Adopted, fix backlog | cppcheck |
| Rule 16.5 - A default label shall appear as either the first or the last switch label of a switch statement | Required | Adopted, clean | cppcheck |
| Rule 16.6 - Every switch statement shall have at least two switch-clauses | Required | Adopted, clean | cppcheck |
| Rule 16.7 - A switch-expression shall not have essentially Boolean type | Required | Adopted, clean | cppcheck |
| Rule 17.3 - A function shall not be declared implicitly | Mandatory | Adopted, clean | cppcheck |
| Rule 17.4 - All exit paths from a function with non-void return type shall have an explicit return statement with an expression | Mandatory | Adopted, enforced by other means | review |
| Rule 17.5 - The function argument corresponding to a parameter declared to have an array type shall have an appropriate number of elements | Advisory | Adopted, enforced by other means | review |
| Rule 17.6 - The declaration of an array parameter shall not contain the static keyword between the [ ] | Mandatory | Adopted, clean | cppcheck |
| Rule 18.1 - A pointer resulting from arithmetic on a pointer operand shall address an element of the same array as that pointer operand | Required | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 18.2 - Subtraction between pointers shall only be applied to pointers that address elements of the same array | Required | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 18.3 - The relational operators >, >=, < and <= shall not be applied to objects of pointer type except where they point into the same object | Required | Adopted, enforced by other means | review |
| Rule 18.6 - The address of an object with automatic storage shall not be copied to another object that persists after the first object has ceased to exist | Required | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 18.7 - Flexible array members shall not be declared | Required | Adopted, clean | cppcheck |
| Rule 18.8 - Variable-length array types shall not be used | Required | Adopted, clean | cppcheck |
| Rule 19.1 - An object shall not be assigned or copied to an overlapping object | Mandatory | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 20.2 - The ', " or \ characters and the /* or // character sequences shall not occur in a header file name | Required | Adopted, clean | cppcheck |
| Rule 20.3 - The #include directive shall be followed by either a <filename> or "filename" sequence | Required | Adopted, clean | cppcheck |
| Rule 20.4 - A macro shall not be defined with the same name as a keyword | Required | Adopted, clean | cppcheck |
| Rule 20.6 - Tokens that look like a preprocessing directive shall not occur within a macro argument | Required | Adopted, enforced by other means | review |
| Rule 20.7 - Expressions resulting from the expansion of macro parameters shall be enclosed in parentheses | Required | Adopted, fix backlog | cppcheck |
| Rule 20.8 - The controlling expression of a #if or #elif preprocessing directive shall evaluate to 0 or 1 | Required | Adopted, clean | cppcheck |
| Rule 20.9 - All identifiers used in the controlling expression of #if or #elif preprocessing directives shall be #define'd before evaluation | Required | Adopted, clean | cppcheck |
| Rule 20.11 - A macro parameter immediately following a # operator shall not immediately be followed by a ## operator | Required | Adopted, clean | cppcheck |
| Rule 20.12 - A macro parameter used as an operand to the # or ## operators, which is itself subject to further macro replacement, shall only be used as an operand to these operators | Required | Adopted, fix backlog | cppcheck |
| Rule 20.13 - A line whose first token is # shall be a valid preprocessing directive | Required | Adopted, clean | cppcheck |
| Rule 20.14 - All #else, #elif and #endif preprocessor directives shall reside in the same file as the #if, #ifdef or #ifndef directive to which they are related | Required | Adopted, clean | cppcheck |
| Rule 21.1 - #define and #undef shall not be used on a reserved identifier or reserved macro name | Required | Adopted, fix backlog | cppcheck |
| Rule 21.2 - A reserved identifier or reserved macro name shall not be declared | Required | Adopted, clean | cppcheck |
| Rule 21.5 - The standard header file <signal.h> shall not be used | Required | Adopted, clean | cppcheck |
| Rule 21.7 - The Standard Library functions atof, atoi, atol and atoll functions of <stdlib.h> shall not be used | Required | Adopted, fix backlog | cppcheck |
| Rule 21.9 - The Standard Library functions bsearch and qsort of <stdlib.h> shall not be used | Required | Adopted, fix backlog | cppcheck |
| Rule 21.11 - The standard header file <tgmath.h> shall not be used | Required | Adopted, clean | cppcheck |
| Rule 21.12 - The exception handling features of <fenv.h> should not be used | Advisory | Adopted, clean | cppcheck |
| Rule 21.13 - Any value passed to a function in <ctype.h> shall be representable as an unsigned char or be the value EOF | Mandatory | Adopted, enforced by other means | review |
| Rule 21.14 - The Standard Library function memcmp shall not be used to compare null terminated strings | Required | Adopted, clean | cppcheck |
| Rule 21.15 - The pointer arguments to the Standard Library functions memcpy, memmove and memcmp shall be pointers to qualified or unqualified versions of compatible types | Required | Adopted, fix backlog | cppcheck |
| Rule 21.16 - The pointer arguments to the Standard Library function memcmp shall point to either a pointer type, an essentially signed type, an essentially unsigned type, an essentially Boolean type or an essentially enum type | Required | Adopted, fix backlog | cppcheck |
| Rule 21.17 - Use of the string handling functions from <string.h> shall not result in accesses beyond the bounds of the objects referenced by their pointer parameters | Mandatory | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 21.18 - The size_t argument passed to any function in <string.h> shall have an appropriate value | Mandatory | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 21.19 - The pointers returned by the Standard Library functions localeconv, getenv, setlocale or, strerror shall only be used as if they have pointer to const-qualified type | Mandatory | Adopted, clean | cppcheck |
| Rule 21.20 - The pointer returned by the Standard Library functions asctime, ctime, gmtime, localtime, localeconv, getenv, setlocale or strerror shall not be used following a subsequent call to the same function | Mandatory | Adopted, clean | cppcheck |
| Rule 22.1 - All resources obtained dynamically by means of Standard Library functions shall be explicitly released | Required | Adopted, enforced by other means | review |
| Rule 22.2 - A block of memory shall only be freed if it was allocated by means of a Standard Library function | Mandatory | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 22.3 - The same file shall not be open for read and write access at the same time on different streams | Required | Adopted, enforced by other means | review |
| Rule 22.4 - There shall be no attempt to write to a stream which has been opened as read-only | Mandatory | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 22.5 - A pointer to a FILE object shall not be dereferenced | Mandatory | Adopted, clean | cppcheck |
| Rule 22.6 - The value of a pointer to a FILE shall not be used after the associated stream has been closed | Mandatory | Adopted, enforced by other means | valgrind, sanitizer, fuzzer |
| Rule 22.7 - The macro EOF shall only be compared with the unmodified return value from any Standard Library function capable of returning EOF | Required | Adopted, clean | cppcheck |
| Rule 22.8 - The value of errno shall be set to zero prior to a call to an errno-setting- function | Required | Adopted, fix backlog | cppcheck |
| Rule 22.9 - The value of errno shall be tested against zero after calling an errno- setting-function | Required | Adopted, fix backlog | cppcheck |
| Rule 22.10 - The value of errno shall only be tested when the last function to be called was an errno-setting-function | Required | Adopted, fix backlog | cppcheck |

## The Mandatory guidelines the checker cannot see

MISRA 6.2.1 permits no deviation from a Mandatory guideline, so these need a
credible enforcement story even though cppcheck has no check for them.

| Guideline | Enforced by |
|---|---|
| Rule 9.1 - object with automatic storage read before being set | valgrind, MemorySanitizer-class checks |
| Rule 12.5 - `sizeof` of a function parameter declared as an array | compiler warning, review |
| Rule 17.4 - non-void function with an exit path and no return | `-Wreturn-type`, which is in `-Wall` |
| Rule 19.1 - object assigned or copied to an overlapping object | valgrind, ASan |
| Rule 21.13 - `<ctype.h>` argument outside `unsigned char` | review; the lexer is the only caller |
| Rule 21.17 - `<string.h>` access beyond the object | ASan, valgrind, fuzzer |
| Rule 21.18 - `<string.h>` `size_t` argument out of range | ASan, valgrind, fuzzer |
| Rule 22.2 - freeing a block that was not allocated | ASan, valgrind, `ast-free-leak-probe` |
| Rule 22.4 - writing to a stream opened read-only | review |
| Rule 22.6 - using a `FILE *` after it is closed | ASan, valgrind |

Most of this list is already covered by gates that exist and run today. That
is the honest reason a Mandatory guideline can be claimed without a MISRA
tool: another instrument checks it, and that instrument has teeth.
