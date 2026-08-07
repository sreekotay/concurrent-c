#!/usr/bin/env python3
"""Sync slice-lit coerce fixes from pp_emit_ufcs.cch into the lowered .h face.

SHADOW_LOWER_SOURCE=ccs host-cc prefers out/include/cc/shadow/*.h over .cch,
so face edits in .cch are invisible until this (or a proper face lower) runs.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CCH = ROOT / "cc/shadow/pp_emit_ufcs.cch"
H = ROOT / "out/include/cc/shadow/pp_emit_ufcs.h"


def main() -> int:
    cch = CCH.read_text()
    t = H.read_text()

    t = t.replace("const_char_to_slice(%.*s)", "CC_SLICE_LIT(%.*s)")
    t = t.replace("const_char_to_slice(%s)", "CC_SLICE_LIT(%s)")
    t = re.sub(r"(?<![_\w])char_to_slice\(%s\)", "CC_SLICE_LIT(%s)", t)
    t = re.sub(r"(?<![_\w])char_to_slice\(%\.\*s\)", "CC_SLICE_LIT(%.*s)", t)
    t = t.replace(
        '"const_char_to_slice", "char_to_slice", "cc_slice_from_cstr"',
        '"cc_slice_from_static", "CC_SLICE_LIT", "cc_slice_cstr",\n'
        '        "char_to_slice_n", "const_char_to_slice_n"',
    )

    m = re.search(
        r"static int shadow_slc_known_arg\(const char\* callee, int arg_index\) \{\n"
        r"(.*?)\n\}",
        cch,
        re.S,
    )
    if not m:
        print("error: known_arg missing in .cch", file=sys.stderr)
        return 1
    new_fn = (
        "static int shadow_slc_known_arg(const char* callee, int arg_index) {\n"
        + m.group(1)
        + "\n}"
    )
    t2, n = re.subn(
        r"static int shadow_slc_known_arg\(const char\* callee, int arg_index\) \{\n"
        r".*?\n\}",
        new_fn,
        t,
        count=1,
        flags=re.S,
    )
    if n != 1:
        print(f"error: known_arg replace failed n={n}", file=sys.stderr)
        return 1
    t = t2

    fwd = (
        "static void shadow_rewrite_slice_lit_call_args(char* expr, size_t cap);\n"
        "static void shadow_rewrite_as_call_args(char* expr, size_t cap);\n"
        "static void shadow_rewrite_slice_as_call_args(char* expr, size_t cap);\n\n"
    )
    marker = "/* Structured UFCS → C via lower_parts (no text reconstruct on the call). */"
    if "static void shadow_rewrite_slice_lit_call_args(char* expr, size_t cap);" not in t.split(
        marker
    )[0]:
        if marker not in t:
            print("error: structured UFCS marker missing", file=sys.stderr)
            return 1
        t = t.replace(marker, fwd + marker, 1)

    old = (
        "    /* Call-arg `=>` lifted onto dbody — splice make() into the lowered call. */\n"
        "    {\n"
        "        AstNode* cl = shadow_expr_closure_kid(st);\n"
        "        if (cl) shadow_splice_closure_arg(dst, cap, cl);\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "/* If src is exactly one UFCS call"
    )
    new = (
        "    /* Call-arg `=>` lifted onto dbody — splice make() into the lowered call. */\n"
        "    {\n"
        "        AstNode* cl = shadow_expr_closure_kid(st);\n"
        "        if (cl) shadow_splice_closure_arg(dst, cap, cl);\n"
        "    }\n"
        "    /* Structured UFCS used to skip lit coerce (only the text-peel path ran\n"
        "     * it) — `return c->err(\"…\")` stayed a bare C string. */\n"
        "    shadow_rewrite_slice_as_call_args(dst, cap);\n"
        "    shadow_rewrite_slice_lit_call_args(dst, cap);\n"
        "    shadow_rewrite_as_call_args(dst, cap);\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "/* If src is exactly one UFCS call"
    )
    if "Structured UFCS used to skip lit coerce" not in t:
        if old not in t:
            print("error: emit_ufcs_to_buf epilogue not found", file=sys.stderr)
            return 1
        t = t.replace(old, new, 1)

    # Sync to_slice_n method (replaces retired strlen-hiding to_slice).
    m = re.search(
        r'    \} else if \(strcmp\(meth_name, "to_slice_n"\) == 0\) \{.*?\n'
        r'    \} else if \(!is_arrow && strcmp\(meth_name, "read_into"\) == 0\) \{',
        cch,
        re.S,
    )
    if m:
        pat = re.compile(
            r'    \} else if \(strcmp\(meth_name, "to_slice(?:_n)?"\) == 0\) \{.*?\n'
            r'    \} else if \(!is_arrow && strcmp\(meth_name, "read_into"\) == 0\) \{',
            re.S,
        )
        t, n = pat.subn(m.group(0), t, count=1)
        if n != 1:
            print(f"warn: to_slice_n sync failed n={n}", file=sys.stderr)

    # Sync lit-call rewrite (nested cc_is_err(f.write("…")) recurse + char[:] want).
    m = re.search(
        r"/\* Wrap string lits at known CCSlice params: cc_path_exists\(\".\"\) etc\. \*/\n"
        r"static void shadow_rewrite_slice_lit_call_args\(char\* expr, size_t cap\) \{\n"
        r"(.*?)\n\}\n\n"
        r"(/\* @as arg coerce:)",
        cch,
        re.S,
    )
    if not m:
        print("error: rewrite_slice_lit_call_args missing in .cch", file=sys.stderr)
        return 1
    new_block = (
        "/* Wrap string lits at known CCSlice params: cc_path_exists(\".\") etc. */\n"
        "static void shadow_rewrite_slice_lit_call_args(char* expr, size_t cap) {\n"
        + m.group(1)
        + "\n}\n\n"
        + m.group(2)
    )
    t2, n = re.subn(
        r"/\* Wrap string lits at known CCSlice params: cc_path_exists\(\".\"\) etc\. \*/\n"
        r"(?:#line[^\n]*\n)?"
        r"static void shadow_rewrite_slice_lit_call_args\(char\* expr, size_t cap\) \{\n"
        r".*?\n\}\n\n"
        r"/\* @as arg coerce:",
        new_block,
        t,
        count=1,
        flags=re.S,
    )
    if n != 1:
        print(f"error: rewrite_slice_lit_call_args replace failed n={n}", file=sys.stderr)
        return 1
    t = t2

    H.write_text(t)
    ok = (
        "CC_SLICE_LIT(%.*s)" in t
        and "const_char_to_slice(%.*s)" not in t
        and "cc_file_write"
        in t[t.find("shadow_slc_known_arg") : t.find("shadow_slc_is_string_lit")]
        and "Structured UFCS used to skip lit coerce" in t
        and 'strcmp(meth_name, "to_slice_n")' in t
        and "Nested calls (e.g. cc_is_err(f.write(" in t
        and 'strcmp(fp->base, "char[:]") == 0' in t
    )
    print("patched", H)
    print("ok" if ok else "WARN: verification incomplete", file=sys.stderr if not ok else sys.stdout)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
