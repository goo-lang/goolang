#ifndef TESTING_LAYERING_B_H
#define TESTING_LAYERING_B_H
#include "testing/layering/a.h"
static inline int layering_b(void) { return layering_a() + 1; }
#endif
