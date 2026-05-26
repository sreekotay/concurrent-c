#ifndef CC_DIAG_H
#define CC_DIAG_H

#include <stddef.h>
#include <stdio.h>

typedef struct CCSourceMap CCSourceMap;

typedef enum CCDiagLevel {
    CC_DIAG_NOTE = 0,
    CC_DIAG_WARNING = 1,
    CC_DIAG_ERROR = 2,
} CCDiagLevel;

typedef enum CCConstructKind {
    CC_CONSTRUCT_NONE = 0,
    CC_CONSTRUCT_CLOSURE,
    CC_CONSTRUCT_ASYNC,
    CC_CONSTRUCT_UFCS,
    CC_CONSTRUCT_AUTOBLOCK,
    CC_CONSTRUCT_DEFER,
    CC_CONSTRUCT_DESTROY,
    CC_CONSTRUCT_UNWRAP_BANG,
    CC_CONSTRUCT_UNWRAP_QMARK,
    CC_CONSTRUCT_CHANNEL_HANDLE,
    CC_CONSTRUCT_CHANNEL_SLICE,
    CC_CONSTRUCT_RESULT_TYPE,
    CC_CONSTRUCT_COMPTIME,
    CC_CONSTRUCT_NURSERY,
    CC_CONSTRUCT_SPAWN,
    CC_CONSTRUCT_ERRHANDLER,
    CC_CONSTRUCT_INCLUDE,
    CC_CONSTRUCT_PROTO_REWRITE,
    CC_CONSTRUCT_PREP,
    CC_CONSTRUCT_COUNT,
} CCConstructKind;

typedef struct CCSourcePos {
    int file_id;
    int line;
    int col;
    size_t byte_off;
} CCSourcePos;

typedef struct CCSourceSpan {
    CCSourcePos start;
    CCSourcePos end;
} CCSourceSpan;

#define CC_SPAN_NONE ((CCSourceSpan){{0, 0, 0, 0}, {0, 0, 0, 0}})

static inline int cc_span_is_none(CCSourceSpan s) {
    return s.start.line == 0 && s.start.col == 0 && s.end.line == 0 && s.end.col == 0;
}

typedef struct CCDiag {
    CCDiagLevel level;
    const char* phase;
    CCConstructKind construct_kind;
    CCSourceSpan span;
    char* message;
    char* hint;
    char* macro_chain; /* optional: "expanded from macro CHAN at config.cch:8" */
} CCDiag;

void cc_diag_init(void);
void cc_diag_shutdown(void);

int cc_diag_register_file(const char* path);
const char* cc_diag_file_path(int file_id);

void cc_diag_emit(CCDiagLevel level,
                  const char* phase,
                  CCConstructKind construct_kind,
                  CCSourceSpan span,
                  const char* msg,
                  const char* hint);

void cc_diag_emitf(CCDiagLevel level,
                   const char* phase,
                   CCConstructKind construct_kind,
                   CCSourceSpan span,
                   const char* fmt, ...);

int cc_diag_error_count(void);
void cc_diag_print_all(FILE* out);
void cc_diag_clear(void);

const char* cc_construct_kind_name(CCConstructKind k);

/* Translate raw TCC diagnostic using source map + optional macro chain text. */
void cc_diag_translate_tcc_error(const char* tcc_msg,
                                 int tcc_line,
                                 int tcc_col,
                                 const char* tcc_file,
                                 CCSourceMap* map,
                                 const char* macro_chain);

/* Debug env helpers (I6) */
int cc_debug_enabled(const char* name);
void cc_debug_log(const char* phase, const char* fmt, ...);

/* --show-lowered=<phase> (I7) */
void cc_diag_set_show_lowered_phase(const char* phase);
const char* cc_diag_get_show_lowered_phase(void);
void cc_diag_maybe_dump_lowered(const char* phase, const char* buf, size_t len);

#endif /* CC_DIAG_H */
