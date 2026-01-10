#include "tcc_bridge.h"

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>

static int cc_try_cc_decl(void) { return 0; }
static int cc_try_cc_stmt(void) { return 0; }

const struct TCCExtParser cc_ext_parser = {
    .try_cc_decl = cc_try_cc_decl,
    .try_cc_stmt = cc_try_cc_stmt,
};
#endif


