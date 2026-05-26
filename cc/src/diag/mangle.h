#ifndef CC_DIAG_MANGLE_H
#define CC_DIAG_MANGLE_H

#include "diag.h"
#include <stddef.h>

/* I4: cc_<construct>__<userident>__line<N>_col<M> */
char* cc_diag_mangle_symbol(CCConstructKind kind,
                            const char* user_ident,
                            int line,
                            int col);

#endif /* CC_DIAG_MANGLE_H */
