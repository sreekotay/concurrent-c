#ifndef CT_QUOTE_UNIT_H
#define CT_QUOTE_UNIT_H
/* Beside the unit, not the working directory. Comptime tcc must search
 * that directory for a quoted include. */
static inline int ct_quote_unit_val(void) { return 41; }
#endif
