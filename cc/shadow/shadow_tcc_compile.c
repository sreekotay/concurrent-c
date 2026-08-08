#include "shadow_tcc_compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libtcc.h>
#ifdef malloc
#undef malloc
#endif
#ifdef free
#undef free
#endif
#ifdef realloc
#undef realloc
#endif
#ifdef strdup
#undef strdup
#endif

static void shadow_tcc_err(void* opaque, const char* msg) {
    (void)opaque;
    if (msg) fprintf(stderr, "shadow_tcc: %s\n", msg);
}

static const char* shadow_tcc_find_lib_dir(const char* prefer, char* buf,
                                          size_t cap) {
    const char* cands[8];
    size_t nc = 0;
    if (prefer && prefer[0]) cands[nc++] = prefer;
    {
        const char* env = getenv("CC_TCC_LIB_PATH");
        if (env && env[0]) cands[nc++] = env;
    }
#ifdef CC_TCC_LIB_DIR
    cands[nc++] = CC_TCC_LIB_DIR;
#endif
    cands[nc++] = "third_party/tcc";
    cands[nc++] = "../third_party/tcc";
    cands[nc++] = "../../third_party/tcc";
    for (size_t i = 0; i < nc; i++) {
        char probe[1024];
        if (snprintf(probe, sizeof(probe), "%s/libtcc1.a", cands[i]) >=
            (int)sizeof(probe))
            continue;
        if (access(probe, R_OK) == 0) {
            snprintf(buf, cap, "%s", cands[i]);
            return buf;
        }
    }
    return NULL;
}

static int shadow_tcc_runtime_present(const char* dir) {
    char probe[1100];
    if (!dir || !dir[0]) return 0;
#if defined(__APPLE__)
    snprintf(probe, sizeof(probe), "%s/libcc_runtime.dylib", dir);
#else
    snprintf(probe, sizeof(probe), "%s/libcc_runtime.so", dir);
#endif
    return access(probe, R_OK) == 0;
}

int shadow_tcc_compile_exe(const char* c_src, const char* out_exe,
                           const char* tcc_lib_dir,
                           const char* const* inc_dirs, int ninc,
                           const char* runtime_lib_dir) {
    char libdir_buf[1024];
    const char* libdir;
    TCCState* s;
    int i;

    if (!c_src || !out_exe || !out_exe[0]) return -1;
    if (!shadow_tcc_runtime_present(runtime_lib_dir)) {
        fprintf(stderr,
                "shadow_tcc: libcc_runtime not found in %s\n",
                runtime_lib_dir ? runtime_lib_dir : "(null)");
        return -1;
    }

    s = tcc_new();
    if (!s) {
        fprintf(stderr, "shadow_tcc: tcc_new failed\n");
        return -1;
    }
    tcc_set_error_func(s, NULL, shadow_tcc_err);

    libdir = shadow_tcc_find_lib_dir(tcc_lib_dir, libdir_buf, sizeof(libdir_buf));
    if (libdir) {
        tcc_set_lib_path(s, libdir);
        tcc_add_library_path(s, libdir);
        {
            char inc[1100];
            snprintf(inc, sizeof(inc), "%s/include", libdir);
            tcc_add_sysinclude_path(s, inc);
        }
    }

    for (i = 0; i < ninc; i++) {
        if (inc_dirs[i] && inc_dirs[i][0])
            tcc_add_include_path(s, inc_dirs[i]);
    }

    tcc_add_library_path(s, runtime_lib_dir);
    tcc_set_options(s, "-std=c11");

    if (tcc_set_output_type(s, TCC_OUTPUT_EXE) < 0) {
        fprintf(stderr, "shadow_tcc: set_output_type EXE failed\n");
        tcc_delete(s);
        return -1;
    }

    if (tcc_compile_string(s, c_src) < 0) {
        fprintf(stderr, "shadow_tcc: compile_string failed\n");
        tcc_delete(s);
        return -1;
    }

    /* Leaf soname only — load via DYLD_LIBRARY_PATH / LD_LIBRARY_PATH. */
    if (tcc_add_library(s, "cc_runtime") < 0) {
        fprintf(stderr, "shadow_tcc: -lcc_runtime failed\n");
        tcc_delete(s);
        return -1;
    }

    tcc_add_library(s, "pthread");
    tcc_add_library(s, "m");

    if (tcc_output_file(s, out_exe) < 0) {
        fprintf(stderr, "shadow_tcc: output_file(%s) failed\n", out_exe);
        tcc_delete(s);
        return -1;
    }

    tcc_delete(s);
    return 0;
}
