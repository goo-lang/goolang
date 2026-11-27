#ifndef GOO_FLOAT_FORMAT_H
#define GOO_FLOAT_FORMAT_H

#include "runtime.h"

// Format a float as Go's strconv.FormatFloat does.
//   fmt   'e', 'f' or 'g' (any other value formats as 'g').
//   prec  digits after the point for 'e' and 'f', significant digits for 'g'.
//         -1 means the shortest text that reads back as the same value.
//   bits  32 or 64: the precision at which "reads back" is judged. A float32
//         arrives widened to double, and bits == 32 keeps its short form
//         (0.1 stays "0.1", not "0.10000000149011612").
// Specials print as Go prints them: "+Inf", "-Inf", "NaN". The result is a
// goo_alloc'd string.
goo_string_t goo_format_float(double v, int32_t fmt, int32_t prec, int32_t bits);

#endif
