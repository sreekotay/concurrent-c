/* Whitelist AST + parser for the SERDES shadow front.
 * Requires pp_tape.cch + pp_stage2.cch + <ccc/std/static_map.h>.
 * Nested stmt lists use AstNode.body / dbody (growable parse-arena tables)
 * so they do not interleave with a parent's kids_storage
 * (see AST_NURSERY_DESTROY / AST_SPAWN_CLOSURE).
 *
 *   pp_ast_core.cch        — keywords, AstNode, Parser, spell helpers
 *   pp_ast_parse_stmt.cch  — statement / expr parsers
 *   pp_ast_parse_ext.cch   — external / TU parsers + parse_tu
 */
#pragma once
#include "pp_ast_core.h"
#include "pp_ast_parse.h"
#include "pp_ast_safety.h"
