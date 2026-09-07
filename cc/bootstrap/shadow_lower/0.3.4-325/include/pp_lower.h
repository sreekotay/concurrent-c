/* Lower: whitelist AST parse + shadow emit (same 2-stage front).
 *
 *   pp_ast*.cch  — keywords, AstNode, parse_tu / parse_external
 *   pp_emit*.cch — CEmit, shadow_emit_c / shadow_emit_h
 *
 * Split by concern so parse and product text can evolve independently.
 * Requires pp_tape.cch + pp_stage2.cch + <ccc/std/static_map.cch>. */
#pragma once
#include "pp_ast.h"
#include "pp_emit.h"
