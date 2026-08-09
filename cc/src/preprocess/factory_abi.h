/* The driver-side mirror of CCSlice for compiled-factory calls.
 *
 * Factory bodies compile at comptime against the REAL CCSlice from the
 * comptime prelude; the driver calls them through this mirror, so the
 * two layouts MUST match field-for-field.  A drift garbles every
 * by-value slice in both directions and the factory quietly returns
 * empty — which is why the factory TU also compiles a probe
 * (CC_FACTORY_ABI_PROBE_SYM returning sizeof(CCSlice)) that the loader
 * checks against sizeof(CCFactorySlice) before any factory runs: a
 * mismatch is a loud load-time error, never a silent empty emit. */
#ifndef CC_FACTORY_ABI_H
#define CC_FACTORY_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef struct CCFactorySlice {
    void*    ptr;
    size_t   len;
    uint64_t id;
} CCFactorySlice;

typedef struct {
    CCFactorySlice* items;
    size_t          len;
} CCFactorySliceArray;

typedef CCFactorySlice (*CCGenericFactoryFn)(CCFactorySlice, CCFactorySlice,
                                             CCFactorySliceArray, void*);

#define CC_FACTORY_ABI_PROBE_SYM "__cc_gfac_slice_abi_size"
#define CC_FACTORY_ABI_PROBE_DEF \
    "size_t __cc_gfac_slice_abi_size(void) { return sizeof(CCSlice); }\n"

#endif /* CC_FACTORY_ABI_H */
