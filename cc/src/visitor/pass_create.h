#ifndef CC_VISITOR_PASS_CREATE_H
#define CC_VISITOR_PASS_CREATE_H

#include <stddef.h>

#include "comptime/symbols.h"

char* cc_rewrite_registered_type_create_destroy(const char* src,
                                                size_t n,
                                                const char* input_path,
                                                CCSymbolTable* symbols);

#endif
