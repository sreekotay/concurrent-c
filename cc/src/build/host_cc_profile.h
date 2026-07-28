/*
 * Host-compiler profile: probe once per CC binary fingerprint, persist under
 * <cache_root>/host/<hash>.profile, reuse flags on later builds.
 *
 * Answers "what flags does this host CC need to compile our lowered C?", not
 * runtime concurrency health.
 */
#ifndef CC_BUILD_HOST_CC_PROFILE_H
#define CC_BUILD_HOST_CC_PROFILE_H

#include <stddef.h>

#define CC_HOST_PROFILE_SCHEMA 1

typedef struct CCHostCcProfile {
    char cc_path[1024];
    char flags[512];     /* leading-space flags: " -std=c11 -B..." */
    int is_tcc;
    int no_liblfds;      /* runtime should define CC_NO_LIBLFDS */
    int no_xjb_float;    /* skip XJB float fmt object */
    int ok;
} CCHostCcProfile;

/* Load or create a profile for `cc_bin`. `cache_root` is typically
 * out/.cc-build; `repo_root` finds third_party/tcc for -B. Returns 0 and
 * fills `out` on success. Set CC_REPROBE_HOST=1 to force a fresh probe. */
int cc_host_cc_profile_ensure(const char* cc_bin,
                              const char* cache_root,
                              const char* repo_root,
                              CCHostCcProfile* out);

/* Append profile.flags onto a compile/link command buffer. */
void cc_host_cc_profile_append_flags(const CCHostCcProfile* p,
                                     char* cmd,
                                     size_t cmd_cap);

/* 16 hex chars + NUL. Fingerprint = resolved path + mtime + size + schema.
 * Host-native objects and compile/link metas live under
 * <cache_root>/host/<fingerprint>/ so CC=tcc cannot reuse clang Mach-O .o. */
int cc_host_cc_fingerprint(const char* cc_bin, char* hex_out, size_t hex_cap);

/* Write <cache_root>/host/<fingerprint> into `out`. cache_root defaults to
 * out/.cc-build when null/empty. */
int cc_host_cc_obj_root(const char* cc_bin,
                        const char* cache_root,
                        char* out,
                        size_t out_cap);

#endif /* CC_BUILD_HOST_CC_PROFILE_H */
