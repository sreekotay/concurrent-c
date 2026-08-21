#pragma once
#ifndef HEADER_UNIT_H
#define HEADER_UNIT_H

#define ID(x) x
#define Z 0
#if ID(Z)
typedef int dead_z;
#else
typedef int live_z;
#endif
#undef Z
#define Z 1
#if Z
typedef int unit_flag;
#endif

static int header_unit_id(int x) { return ID(x); }
typedef struct UnitTag UnitTag;
struct UnitFwd;
struct UnitRec {
    int rec_x;
};
int header_unit_proto(int x);
#ifdef __cplusplus
extern "C" {
#endif
int header_unit_c(void);
#ifdef __cplusplus
}
#endif

enum { UNIT_LEAF = 1, UNIT_OBJ = 2 };
enum UnitKind { UNIT_A = 0, UNIT_B };
static const unsigned char unit_tab[4] = { 1, 0, 1, 0 };
extern int unit_ext;
typedef struct { alignas(16) char unit_pad[16]; } UnitAlign;
typedef struct { const UnitTag *unit_ptr; size_t a, b, c; } UnitPtr;
typedef struct {
    union { int unit_u_a; int unit_u_b; } unit_u;
} UnitNest;
typedef struct { CCJ_ALIGNAS UnitTag unit_align_fld; } UnitAlignPrefix;
int !>(int) unit_bang(void);
@typeview on UnitRec { as: rec_x; };
#define UNIT_DECL(a) typedef int a
UNIT_DECL(unit_decl_t)
#ifdef UNIT_NEVER
#endif
#ifdef __cplusplus
template<typename ty> ty unit_id(ty x) { return x; }
static int unit_cxx_mid =
    1
#if __cplusplus >= 202101L
    || 2
#endif
    ;
#endif

#endif
