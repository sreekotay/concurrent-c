/* Shortest float/double formatting via vitaut/zmij, plus trailing `.0` polish
 * so every finite result contains '.' or 'e' (stdlib string policy). */
#include <stddef.h>
#include <string.h>

/* TinyCC's float.h omits C11 DECIMAL_DIG macros; zmij needs them. */
#ifndef DBL_DECIMAL_DIG
#define DBL_DECIMAL_DIG 17
#endif
#ifndef FLT_DECIMAL_DIG
#define FLT_DECIMAL_DIG 9
#endif

/* TCC lacks the NEON/SSE paths zmij uses by default. */
#ifdef __TINYC__
#ifndef ZMIJ_USE_SIMD
#define ZMIJ_USE_SIMD 0
#endif
#endif

#if __has_include("vendor/zmij-c.h")
#include "vendor/zmij-c.h"
#elif __has_include("../../third_party/zmij/zmij-c.h")
#include "../../third_party/zmij/zmij-c.h"
#elif __has_include("../../../third_party/zmij/zmij-c.h")
#include "../../../third_party/zmij/zmij-c.h"
#else
#error "zmij-c.h not found"
#endif

#if __has_include("vendor/zmij.c")
#include "vendor/zmij.c"
#elif __has_include("../../third_party/zmij/zmij.c")
#include "../../third_party/zmij/zmij.c"
#elif __has_include("../../../third_party/zmij/zmij.c")
#include "../../../third_party/zmij/zmij.c"
#else
#error "zmij source not found"
#endif

/* Append ".0" when fixed notation has no decimal point and no exponent. */
static char *cc__float_polish_dot0(char *buf, char *end) {
    const char *p;
    if (!buf || !end || end < buf) return end;
    for (p = buf; p < end; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E') return end;
    }
    /* Non-finite literals: nan / inf / -inf — leave unchanged. */
    if (end - buf >= 3) {
        if ((buf[0] == 'n' || buf[0] == 'N') &&
            (buf[1] == 'a' || buf[1] == 'A') &&
            (buf[2] == 'n' || buf[2] == 'N'))
            return end;
        if ((buf[0] == 'i' || buf[0] == 'I') &&
            (buf[1] == 'n' || buf[1] == 'N') &&
            (buf[2] == 'f' || buf[2] == 'F'))
            return end;
    }
    if (end - buf >= 4 && buf[0] == '-' &&
        (buf[1] == 'i' || buf[1] == 'I') &&
        (buf[2] == 'n' || buf[2] == 'N') &&
        (buf[3] == 'f' || buf[3] == 'F'))
        return end;
    end[0] = '.';
    end[1] = '0';
    return end + 2;
}

char *cc_zmij_f64_to_string(double v, char *buf) {
    char *end = zmij_detail_write_double(v, buf);
    return cc__float_polish_dot0(buf, end);
}

char *cc_zmij_f32_to_string(float v, char *buf) {
    char *end = zmij_detail_write_float(v, buf);
    return cc__float_polish_dot0(buf, end);
}
