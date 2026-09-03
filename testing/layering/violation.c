/* Includes a.h, which arrives through :b transitively and is NOT a direct dep.
   That is exactly what layering_check exists to refuse. */
#include "testing/layering/a.h"
#include "testing/layering/b.h"
int layering_violation(void) { return layering_a() + layering_b(); }
