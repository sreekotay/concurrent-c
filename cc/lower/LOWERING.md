# Lowering shapes

The C each Concurrent-C construct lowers to. The runtime and stdlib do not
change, so these are the shapes the current lowerer emits, written down so
the clean lowerer reproduces the behaviour without reproducing the code.
Every generated line is pinned to the user's line of the construct with a
`#line`; the printer does that from the spans, so nothing here emits
`#line` by hand.

## Results

**Result types.** `T!>(E)` is `CCResult_T_E`, `T?>(E)` the same type with
the optional-discard property recorded in the index. Canonical spelling
mangles the value type: `int`, `size_t`, `void`, `CCSlice`, `voidptr` for
`void*`, `charptr` for `char*`, `unsigned_charptr`, `CCPyptr`, `intptr_t`.
Each spec the TU uses and no included header declares is emitted once at
the top of the product, after the includes:

```c
#line 1 "<cc:result-specs>"
#ifndef CCResult_int_CCError_DEFINED
#define CCResult_int_CCError_DEFINED 1
#ifdef CC_DECL_RESULT_SPEC
CC_DECL_RESULT_SPEC(CCResult_int_CCError, int, CCError)
#else
typedef struct { int ok; union { int value; CCError error; } u; } CCResult_int_CCError;
static inline CCResult_int_CCError cc_ok_CCResult_int_CCError(int v) { CCResult_int_CCError r; r.ok = 1; r.u.value = v; return r; }
static inline CCResult_int_CCError cc_err_CCResult_int_CCError(CCError e) { CCResult_int_CCError r; r.ok = 0; r.u.error = e; return r; }
#endif
#endif
```

`void` value uses `CC_DECL_RESULT_SPEC_VOID(name, E)` and a `cc_ok_...(void)`.

**Constructors.** In a function declared `T!>(E)`: `cc_ok(v)` →
`cc_ok_CCResult_T_E(v)`, `cc_ok(void)` → `cc_ok_CCResult_void_E()`,
`cc_err(e)` → `cc_err_CCResult_T_E(e)`, `cc_err(CC_ERR_X, "msg")` →
`cc_err_CCResult_T_E(CC_ERROR(CC_ERR_X, "msg"))`. Explicit forms
`cc_ok(T, v)`, `cc_ok(T, E, v)`, `cc_err(T, e)`, `cc_err(T, E, e)` name
the spec directly. When the error value's type is not `E`, it is projected
through the `as:` face graph; the current shape is a `_Generic` ladder over
the declared faces:

```c
cc_err_CCResult_int_CCError(({ __typeof__(e) __cc_ep = (e); _Generic(__cc_ep, CCError: __cc_ep, CCIoError: (*(CCError*)(void*)&__cc_ep), default: __cc_ep); }))
```

The clean lowerer emits the same projection but builds the ladder from the
faces the index found on the declarations, not from a fixed list.

**Statement `e !>;` with an `@errhandler(E h)` in scope** (initializer form
`int total = e !>;` shown; the expression-statement form has no `total`):

```c
int total;
{
    __typeof__(get_total_wait_time()) __r = get_total_wait_time();
    if (!__r.ok) {
        cc_rt_diag_record_unwrap_site("recipe.ccs", "49");
        __cc_eh_e_0 = _Generic((__r).u.error, CCError: (__r).u.error, CCIoError: (*(CCError*)(void*)&(__r).u.error), default: (__r).u.error);
        goto __cc_eh_0;
    }
    total = (__r).u.value;
}
```

The handler is hoisted to the end of the function body as a label, with the
error cell declared at the top of the function:

```c
    CCError __cc_eh_e_0;              /* at function top */
    ...
    return 0;
__cc_eh_0:;
    {
        CCError e = __cc_eh_e_0;
        cc_error_log(e);
        return 1;
    }
```

A handler whose statement does not diverge (no `return`, `goto`, `exit`,
`abort`, `cc_error_exit`, `longjmp`; `_Noreturn` on the declaration decides,
with the index) is emitted inline at the unwrap site instead of hoisted.
One cell and label per handler; handlers are selected by the Result's error
type `E`, innermost first; no handler for `E` in scope is an error at the
`!>` naming `E` and the handlers that are in scope.

**`e !> body` and `e !>(err) body`:**

```c
int timeout;
{
    __typeof__(read_config_value("timeout")) __r = read_config_value("timeout");
    if (!__r.ok) {
        __typeof__(__r.u.error) e = __r.u.error;   /* only with a binder */
        <body>                                      /* must diverge at expression position */
    }
    timeout = __r.u.value;
}
```

`@err(e);` inside the body is `__cc_eh_e_N = (e); goto __cc_eh_N;` for the
handler matching `e`'s type.

**`e ?> default` and `e ?>(err) default`:**

```c
int missing;
{
    __typeof__(read_config_value("k")) __r = read_config_value("k");
    missing = !__cc_uw_is_err(__r) ? __cc_uw_value(__r) : (-1);
}
```

```c
int bad;
{
    __typeof__(read_config_value("")) __r = read_config_value("");
    if (__r.ok) { bad = __r.u.value; }
    else { __typeof__(__r.u.error) e = __r.u.error; bad = (<default>); }
}
```

`__cc_uw_is_err`, `__cc_uw_value` and `__cc_uw_err_at` are `_Generic`
ladders over every Result spec the TU uses, emitted after the specs; the
clean lowerer uses `__r.ok` / `__r.u.value` directly and drops the ladders.

**`?>` discard.** A call whose declared type is `T?>(E)` used as a bare
expression statement is `(void)f(...)`; used with `!>` it is the ordinary
unwrap.

**`e !> @destroy;` and `@destroy { D }` on a declaration:** the unwrap as
above; then the variable is registered for destruction at scope exit with
the type's destroy hook from the index (`cc_arena_destroy`, a
`@typehooks .destroy`, the block `D`). Destruction runs in reverse order at
every exit of the scope: fallthrough, `return`, `break`, `continue`,
`goto` out of the scope, and the handler `goto`. `@detach` suppresses it.

**`@defer stmt`, `@defer(ok) stmt`, `@defer(err) stmt`:** same scope-exit
machinery; `(ok)` runs only on a `return cc_ok(...)` or fallthrough of a
Result function, `(err)` only on `return cc_err(...)` or a handler exit.
`@cancel_defer name;` clears the named entry. The current lowerer keeps a
per-function `__cc_defer_hw` high-water counter and a cleanup label
(`goto_cleanup`); the clean lowerer emits the deferred statements inline
at each exit, in reverse order, which is what the `#line` pinning needs.

**Result methods.** `r.is_ok()` → `(r).ok`, `r.is_err()` → `!(r).ok`,
`r.value()` → `((r).ok ? (r).u.value : (cc_error_exit_result(...), (r).u.value))`,
`r.error()` symmetric, `r.unwrap_or(d)` → `((r).ok ? (r).u.value : (d))`.
These come from the index as the method set of every `CCResult_*` type.

## UFCS

**The operator follows the receiver.** A pointer receiver takes `->`, a
value receiver takes `.`; writing the other one is a diagnostic, never a
silent coercion. A char pointer is the one receiver a `.` reaches
through, so `"s".len()` keeps working. Whether the receiver is a pointer
is asked of the type as written, through aliases and through the
pointer-instance rule: a generic instance whose factory hands back a
pointer is spelled as one in every unit that names it, including a
header the current unit only includes.

**The call.** `callee(<recv>, args)` with the receiver fitted to the
callee's first parameter: by address when the callee takes a pointer and
the receiver is a value, itself when the two agree. An rvalue receiver
that the callee wants by address lives in a compound literal of its own
type. `Type.fn(args)` is the declared `Type_fn(args)`.

**The bare-name tier.** A plain `m(T, ...)` is callable as `x.m(...)`
under one rule: the address of a receiver may be taken, but a pointer is
never dereferenced and const is never dropped. So a `T*` receiver does
not reach `m(T)`, a `const T*` receiver does not reach `m(T*)`, and
`void*` is a dispatch key for a pointer receiver only. A near miss lands
in the resolution ladder:

```
no UFCS method 'get_x' for receiver type 'Pair'; tried: Pair_get_x pair_get_x; candidate get_x (bare): declared, but first parameter 'Pair* p1' does not take 'Pair'
```

The named tiers (`T_m`, `cc_<snake>_m`, hooks, registrations) are
conventions the declaration opted into, so they keep the ordinary
address rule.

**`as:` faces.** A method resolved through a `@typeview on T { as: f; }`
field belongs to the field, not to `T`: the resolution records the member
chain it walked, dot-joined across hops, and the call site projects the
receiver through it before the address rule applies. `w.create(p)`
resolved through `as: file` is `cc_file_create(&w.file, p)`.

**Declaration checks.** A `@typeview` is checked where it is written, not
where it is used. Each `as:` field names a value member, never a pointer;
the face graph is acyclic; and no type is reachable through two faces,
which would make a projection ambiguous. In a restrict list (`r:`, `w:`,
`rw:`) every name matches a field or a method of the type, with the
nearest name offered as a note when it does not; a `*` anywhere in an
item is a pattern, so `out_*` and `*_len` match the members they name;
and `^*` is refused as ill-formed rather than read as denying nothing.

## Slices

**The type.** `T[:]` is an instance of the slice family, named by the same
canonical spelling the index computes: `CCSlice` for the char family,
`CCSlice_T` otherwise, and `CCSliceUnique` for a unique char slice. A
typed instance is `struct { CCSlice base; }`, and the header declares
`@typeview on CCSlice_* { as: base; }`, so `xs.len` reaches `xs.base.len`
by the ordinary rule that a field the type does not have may live in an
`as:` embed. Nothing in the lowerer names a slice to make that work.

**The value.** A string literal that initializes, is returned as, or is
passed to a slice becomes `CC_SLICE_LIT(...)`, which carries the bytes
and their length with no run of `strlen`. `@slice("...")` is the same
literal in expression position. A braced list is the array it spells,
hoisted to its own local, and a slice over that array, so the storage and
the view share one extent:

```c
char __cc_sl_br[] = { 'a', 'b', 'c', 'd' };
CCSlice br = cc_slice_from_buffer(__cc_sl_br, sizeof(__cc_sl_br)/sizeof(__cc_sl_br[0]));

double __cc_sl_xs[] = { 1.0, 2.0 };
CCSlice_double xs = CCSlice_double_from_buffer(__cc_sl_xs, sizeof(__cc_sl_xs)/sizeof(__cc_sl_xs[0]));
```

**Arguments.** A typed instance handed to a parameter declared as the
erased `CCSlice` becomes `CCSlice_T_bytes(&arg)`, whose length is scaled
by the element size. This happens only where the callee's declaration
provably takes the erased slice: erasing by default would turn a typed
borrow into a byte marshal that still compiles. The coercion runs after
UFCS, when a method call is a plain call and its parameter types are the
callee's.

The conversions the stdlib declares on a `.cast` hook (a char pointer or
a `CCString` to a byte slice) are computed by the hook body, so they wait
on the comptime seam rather than being written into the lowerer.

## Variants

**Declaration.** `@variant V { a: A; b: B; c: void; }` is the tag enum and
the tagged union; a void arm has no union member, and a variant whose arms
are all void has no union:

```c
typedef enum { V_a, V_b, V_c } VKind;
typedef struct V { VKind kind; union { A a; B b; } u; } V;
```

When an arm type carries a registered destroy chain (a `@typehooks`
`.destroy`, or a value member with one; the `Type_destroy` naming
convention does not count here), the drop helper follows the typedef and is
the variant's own destroy hook, so `@destroy`, `x.destroy()` and the
chains of enclosing structs run it:

```c
static inline void V__cc_drop(V* __v) {
    switch (__v->kind) {
    case V_a: { a_destroy(&__v->u.a); } break;
    default: break;
    }
}
```

**Construction.** A designated initializer naming one arm gets its tag;
the arm value moves under `.u`; a void arm keeps only the tag; `.kind =`
passes through. The same shape serves a declaration, a compound literal,
and an initializer nested in a struct or array initializer:

```c
V v = { .kind = V_a, .u.a = make_a() };
V w = { .kind = V_c };
h = (Hold){ .cell = { .kind = V_b, .u.b = 9 } };
```

A bare `.arm` resolves to `V_arm` from the type of what it is compared
with or assigned to (`x.kind == .a`, `VKind k = .a`, `k = .b`).

**Transition.** `x = { .b = e };` and `x = (V){ .b = e };` on a variant
with a drop helper build the new value first, drop the old arm, then
store; on a variant without one they are the plain assignment:

```c
{ V __cc_vt1 = (V){ .kind = V_b, .u.b = e }; V__cc_drop(&(x)); x = __cc_vt1; }
```

A local of a variant with a drop helper is a `@destroy` site for the
cleanup step; one declared without a value starts with a tag past the
arms, `V x = { .kind = (VKind)2 };`, so its first store and its scope
exit drop nothing.

**Projection.** `x.a` is `x.u.a` where a dominating check protects it: the
case of a checked switch on `x`, or the then-branch of `if (x.kind ==
.a)` / `if (x.kind == V_a)` (either operand order). With its own handler or
fallback it needs none:

```c
int64_t n = ({ if ((x).kind != V_a) { return -1; } (x).u.a; });
int64_t m = ({ __typeof__((x).u.a) __cc_pj1; if ((x).kind == V_a) { __cc_pj1 = (x).u.a; } else { __cc_pj1 = (fallback); } __cc_pj1; });
```

In the handler body and the fallback of a two-armed variant the other
arm is protected. Anything else is a diagnostic at the `.`: an
unprotected projection, a write to `.kind`, a reach into `.u`.

**Checked switch.** `@switch (x)` on a value or pointer switches on the
tag; `case .a:` is `case V_a:`; `case .a(bind):` opens a block that
declares the binding from the subject and holds the statements up to the
next label. Every arm must appear unless a `default:` forfeits the check;
`switch (x.kind)` with `V_a` labels is checked the same way.

```c
switch ((r->del).kind) {
    case Del_text: {
        Buf buf = (r->del).u.text;
        ...
    }
    case Del_pieces:
        ...
}
```

`@variant(packed)` is not lowered by the clean lowerer yet: the
declaration is a diagnostic.

## String templates

`@string(`...`, arena)` builds a CCString by pushing each piece in order,
so it is a run of statements and is written where statements go: the
initializer of a declaration. Anywhere else it is a diagnostic naming the
fix, never a partial build.

```c
CCString s = cc_string_new();
cc_string_push_buffer(&s, "hi ", 3, a);
cc__string_slot_push(&s, (who), a);
```

A literal run carries the bytes as the source wrote them and the count of
characters they stand for, so an escape counts once. `@scratch` as the
arena names one stack arena per function, declared at the top and sized
to the largest request in the body:

```c
cc_arena_stack(__cc_str_scratch, 1024);
```

`@string(`...`)` with no arena is the same pieces over a buffer sized on
the stack, which is one expression and goes anywhere:

```c
CCSlice t = cc__string_stack_slice(cc__string_stack_push(cc__string_stack_lit(
    cc__string_stack_new((char[0u + 4u + cc__string_stack_bound((n))]){0},
                         0u + 4u + cc__string_stack_bound((n))), "a\nb ", 4), (n)));
```

The step runs after UFCS and the slice arguments, so a slot expression is
already the C it will be: the stack form spells each slot into its size
expression, and spelling it earlier would freeze a call the later steps
had not rewritten yet. A tagged slot (`$~tag{e}`) and the direct form
`@string(e, arena)` are diagnostics for now rather than a dropped tag.

## Scratch and templates

`@string(\`text ${x} more\`, arena)`:

```c
({ CCArena __cc_tpl_arena = CC__ARENA_HANDLE(arena);
   CCString __cc_tpl = cc_string_new();
   cc_string_push_buffer(&__cc_tpl, "text ", 5, __cc_tpl_arena);
   cc__string_slot_push(&__cc_tpl, (x), __cc_tpl_arena);
   cc_string_push_buffer(&__cc_tpl, " more", 5, __cc_tpl_arena);
   __cc_tpl; })
```

printed one push per line. `@scratch` as the arena is the function's
`cc_arena_stack(__cc_str_scratch, N)` declared at the top of the function
(N = max `@scratch(N)`, default 1024). A statement that consumes a
`@scratch` template in a call and binds nothing is wrapped:

```c
{
    CcArenaCheckpoint __cc_scratch_cp1 = cc_arena_checkpoint_local(__cc_str_scratch);
    <statement>
    cc_arena_restore_local(__cc_scratch_cp1);
}
```

with the restore also emitted before every exit from inside the statement.

## Print

`println(x)` is `cc_println(x)`; the alias list is the set of functions
declared with the print attribute in `stdio.cch`. A bare `println(...)`
statement is a discarded optional Result.
