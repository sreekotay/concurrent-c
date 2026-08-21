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

#endif
