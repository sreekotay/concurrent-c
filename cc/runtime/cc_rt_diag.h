#ifndef CC_RT_DIAG_H
#define CC_RT_DIAG_H

#include <stdint.h>

typedef struct CCRuntimeSourceLoc {
    const char* file;
    int line;
    int col;
    const char* construct;
    const char* user_name;
} CCRuntimeSourceLoc;

/* R0: load serialized source map companion (.ccs.map) */
int cc_rt_diag_load_map(const char* map_path);

int cc_rt_resolve_pc(void* pc, CCRuntimeSourceLoc* out);

void cc_rt_diag_set_async_name(const char* cc_name, const char* file, int line);
void cc_rt_diag_set_channel_meta(const char* name, const char* topology,
                                 const char* file, int line);

#endif
