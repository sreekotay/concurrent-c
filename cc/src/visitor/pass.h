#ifndef CC_VISITOR_PASS_H
#define CC_VISITOR_PASS_H

#include "comptime/symbols.h"

// Pass 0: collect consts/comptime functions.
int cc_run_const_pass(const char* input_path, CCSymbolTable* symbols);

// Pass 1: main visitor (types, semantics, codegen). For now, stub copies input->output.
int cc_run_main_pass(const char* input_path, CCSymbolTable* symbols, const char* output_path);

#endif // CC_VISITOR_PASS_H

