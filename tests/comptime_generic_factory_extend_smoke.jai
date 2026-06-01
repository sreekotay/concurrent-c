// Illustrative Jai equivalent of tests/comptime_generic_factory_extend_smoke.ccs
// (not run by the CC test harness — reference / comparison only)
//
// CC model                          Jai model
// -------------------------------   ------------------------------------------
// CC_GENERIC_FACTORY(Box)           struct(T) — type shell
// CC_GENERIC_FACTORY_EXTEND         polymorphic procs in other modules / files
// return cc_slice_empty()           compile-time if / compile_error — no symbol
// Box::[int] / Box_int_make         Box(int), box_make(..., int)
// emitted C artifact                monomorph at compile time (no separate .c)

#import "Basic";

// --- "base factory" (box_base.jai) -----------------------------------------
Box :: struct(T: Type) {
    value: T;
}

// --- "extend: make" (box_make.jai — no edit to Box) ------------------------
box_make :: (v: T, $T: Type) -> Box(T) {
    b: Box(T);
    b.value = v;
    return b;
}

// --- "extend: get" (box_get.jai) -------------------------------------------
box_get :: (b: Box(T), $T: Type) -> T {
    return b.value;
}

// --- "conditional extend: is_int" (box_int.jai) ----------------------------
// CC: emit `_is_int` only for Box::[int]; empty slice for all other T.
// Jai: compile-time branch — call only for int, or hard-error if misused.
box_is_int :: ($T: Type) -> int {
    if T == int {
        return 1;
    }
    #assert false;  // CC: symbol not emitted; Jai: ill-formed if instantiated
    return 0;
}

// --- main (same checks as the .ccs smoke) ----------------------------------
main :: () {
    a := box_make(42, int);
    d := box_make(2.5, float64);

    if box_get(a, int) != 42        return 1;
    if box_get(d, float64) != 2.5   return 2;
    if box_is_int(int) != 1         return 3;

    print("comptime_generic_factory_extend_smoke ok a=% d=% int=%\n",
          box_get(a, int), box_get(d, float64), box_is_int(int));
}
