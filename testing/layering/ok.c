/* Includes only its DIRECT dep's header. Must always build. */
#include "testing/layering/b.h"
int layering_ok(void) { return layering_b(); }
