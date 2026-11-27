// Go-compatible float formatting (strconv.FormatFloat, shortest and fixed
// precision). It lives apart from runtime.c because the digit-selection and
// layout rules are a unit of their own, and Task 17 (strconv.FormatFloat)
// calls the same entry point as fmt does.
#include "float_format.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Digits that a 64-bit value can need to read back exactly, minus one for the
// leading digit that "%e" prints before the point.
enum { FF_SHORTEST_MAX_P64 = 16, FF_SHORTEST_MAX_P32 = 8 };
// A caller-supplied precision above this is clamped. C prints the exact binary
// expansion past 17 digits, and 767 significant digits cover every double.
enum { FF_MAX_PREC = 400 };
// "-d." + FF_MAX_PREC digits + "e+308" + NUL, with slack.
enum { FF_TMP = 512 };
// Layout buffer: the longest shortest-'f' text is 5e-324 as 0.000...5, 330 bytes.
enum { FF_OUT = 1024 };
// Verb letters as int32_t, the type of the fmt parameter, so a comparison has
// operands of one essential type.
static const int32_t FF_VERB_E = 101;
static const int32_t FF_VERB_F = 102;

typedef struct {
    char digits[FF_TMP];
    int nd;      // number of digits kept, trailing zeros removed (at least 1)
    int exp10;   // decimal exponent of the first digit, as "%e" prints it
} ff_decimal_t;

static int ff_reads_back(const char* text, double v, int bits) {
    errno = 0;
    double back = (bits == 32) ? (double)strtof(text, NULL) : strtod(text, NULL);
    // Overflow to infinity is the one error that matters here. Underflow to a
    // denormal also sets ERANGE but the value is still compared exactly.
    if (errno == ERANGE && isinf(back)) {
        return 0;
    }
    return back == v;
}

// Split "d.ddde+XX" into digits and exponent.
static void ff_split(const char* text, ff_decimal_t* out) {
    int nd = 0;
    const char* p = text;
    while (*p != '\0' && *p != 'e') {
        if (*p != '.') {
            GOO_ASSERT(nd < FF_TMP - 1);
            out->digits[nd++] = *p;
        }
        p++;
    }
    GOO_ASSERT(*p == 'e');
    // The exponent is "e", a sign and digits, so it is read by hand: strtol
    // would need the errno protocol for a value that always fits.
    p++;
    int neg = 0;
    if (*p == '-' || *p == '+') {
        if (*p == '-') {
            neg = 1;
        }
        p++;
    }
    int e = 0;
    while (*p >= '0' && *p <= '9') {
        e = (e * 10) + (*p - '0');
        p++;
    }
    out->exp10 = neg ? -e : e;
    while (nd > 1 && out->digits[nd - 1] == '0') {
        nd--;
    }
    out->digits[nd] = '\0';
    out->nd = nd;
}

// Digits of |v| (finite). sig < 0 selects the shortest round-trip text.
static void ff_decimal(double v, int sig, int bits, ff_decimal_t* out) {
    char tmp[FF_TMP];
    if (v == 0.0) {
        out->digits[0] = '0';
        out->digits[1] = '\0';
        out->nd = 1;
        out->exp10 = 0;
        return;
    }
    if (sig >= 0) {
        int n = snprintf(tmp, sizeof(tmp), "%.*e", sig - 1, v);
        GOO_ASSERT(n > 0 && (size_t)n < sizeof(tmp));
        ff_split(tmp, out);
        return;
    }
    int max_p = (bits == 32) ? FF_SHORTEST_MAX_P32 : FF_SHORTEST_MAX_P64;
    for (int p = 0; p <= max_p; p++) {
        int n = snprintf(tmp, sizeof(tmp), "%.*e", p, v);
        GOO_ASSERT(n > 0 && (size_t)n < sizeof(tmp));
        if (p == max_p || ff_reads_back(tmp, v, bits)) {
            break;
        }
    }
    ff_split(tmp, out);
}

typedef struct {
    char* buf;
    size_t len;
} ff_out_t;

static void ff_put(ff_out_t* o, char c) {
    GOO_ASSERT(o->len < FF_OUT - 1);
    o->buf[o->len++] = c;
}

// d[.ddd]e+XX with at least two exponent digits, as Go prints it.
static void ff_layout_e(ff_out_t* o, const ff_decimal_t* d, int frac) {
    ff_put(o, d->digits[0]);
    if (frac > 0) {
        ff_put(o, '.');
        for (int i = 1; i <= frac; i++) {
            ff_put(o, i < d->nd ? d->digits[i] : '0');
        }
    }
    ff_put(o, 'e');
    int e = d->exp10;
    ff_put(o, e < 0 ? '-' : '+');
    if (e < 0) {
        e = -e;
    }
    if (e >= 100) {
        ff_put(o, (char)('0' + e / 100));
    }
    ff_put(o, (char)('0' + (e / 10) % 10));
    ff_put(o, (char)('0' + e % 10));
}

// Plain decimal with exactly the digits the value has.
static void ff_layout_f(ff_out_t* o, const ff_decimal_t* d) {
    int dp = d->exp10 + 1;  // digits before the point
    if (dp <= 0) {
        ff_put(o, '0');
    } else {
        for (int i = 0; i < dp; i++) {
            ff_put(o, i < d->nd ? d->digits[i] : '0');
        }
    }
    if (d->nd > dp) {
        ff_put(o, '.');
        for (int i = dp; i < d->nd; i++) {
            if (i < 0) {
                ff_put(o, '0');
            } else {
                ff_put(o, d->digits[i]);
            }
        }
    }
}

static goo_string_t ff_result(const char* buf, size_t len) {
    return goo_string_new_with_length(buf, len);
}

goo_string_t goo_format_float(double v, int32_t fmt, int32_t prec, int32_t bits) {
    if (isnan(v)) {
        return ff_result("NaN", 3);
    }
    if (isinf(v)) {
        return v > 0.0 ? ff_result("+Inf", 4) : ff_result("-Inf", 4);
    }
    GOO_ASSERT(bits == 32 || bits == 64);

    if (prec > FF_MAX_PREC) {
        prec = FF_MAX_PREC;
    }
    char sbuf[FF_OUT];
    ff_out_t o = { sbuf, 0 };
    int negative = 0;
    if (signbit(v)) {
        negative = 1;
    }
    if (negative) {
        ff_put(&o, '-');
        v = -v;
    }

    if (prec >= 0 && fmt == FF_VERB_F) {
        // C and Go agree on fixed-point text. The value is at most 1.8e308, so
        // 309 integer digits plus FF_MAX_PREC fraction digits fit in a heap copy.
        char big[FF_TMP + 320];
        int n = snprintf(big, sizeof(big), "%.*f", prec, v);
        GOO_ASSERT(n > 0 && (size_t)n < sizeof(big));
        size_t sign = negative ? 1U : 0U;
        size_t total = sign + (size_t)n;
        char* all = (char*)malloc(total + 1U);
        if (all == NULL) {
            goo_panic("goo_format_float: out of memory");
        }
        if (negative) {
            all[0] = '-';
        }
        memcpy(all + sign, big, (size_t)n);
        goo_string_t r = ff_result(all, total);
        free(all);
        return r;
    }

    ff_decimal_t d;
    if (fmt == FF_VERB_E) {
        ff_decimal(v, prec < 0 ? -1 : prec + 1, bits, &d);
        ff_layout_e(&o, &d, prec < 0 ? d.nd - 1 : prec);
    } else if (fmt == FF_VERB_F) {
        ff_decimal(v, -1, bits, &d);
        ff_layout_f(&o, &d);
    } else {
        // 'g'. Go's rule: exponent form when the exponent is below -4 or at or
        // above eprec. Shortest text uses 6 for eprec. A given precision uses
        // that precision, lowered to the digit count when all digits sit before
        // the point.
        int sig = prec;
        if (sig == 0) {
            sig = 1;
        }
        ff_decimal(v, sig, bits, &d);
        int eprec = (prec < 0) ? 6 : sig;
        if (prec >= 0 && eprec > d.nd && d.nd >= d.exp10 + 1) {
            eprec = d.nd;
        }
        if (d.exp10 < -4 || d.exp10 >= eprec) {
            ff_layout_e(&o, &d, d.nd - 1);
        } else {
            ff_layout_f(&o, &d);
        }
    }
    return ff_result(sbuf, o.len);
}
