#ifndef CC_COMPTIME_SYMBOLS_H
#define CC_COMPTIME_SYMBOLS_H

#include <stddef.h>

// Minimal symbol table API for comptime constants/functions.
// This will grow into the real comptime environment; for now it
// supports predefined const bindings (e.g. future build.cc outputs).

typedef struct CCSymbolTable CCSymbolTable;

// Simple name/value binding used to preload consts from the driver.
typedef struct {
    const char* name;
    long long value;
} CCConstBinding;

CCSymbolTable* cc_symbols_new(void);
void cc_symbols_free(CCSymbolTable* t);

// Add a const integer value (last writer wins on duplicate names).
int cc_symbols_add_const(CCSymbolTable* t, const char* name, long long value);

// Bulk-add predefined consts; convenience for driver/build integration.
int cc_symbols_add_predefined(CCSymbolTable* t, const CCConstBinding* bindings, size_t count);

// Lookup const; returns 0 on success, non-zero on miss.
int cc_symbols_lookup_const(CCSymbolTable* t, const char* name, long long* out_value);

#endif // CC_COMPTIME_SYMBOLS_H

