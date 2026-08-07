# Restricted access (`@restricted`)

Status: draft — implemented in shadow_lower (tests/restricted_*).

## 1. Notion

A restriction is named, allow-listed access to the same object. It generalizes
`const`: the pointer identity is unchanged; the set of available fields and
UFCS methods shrinks; narrowing (full → restricted) is always implicit with
no cast; widening (restricted → full) never occurs. Zero runtime cost — a
type-system view, not a second object. Restrictions are erased in lowering;
they do not exist at run time.

Channel split handles (`T[~ >]` / `T[~ <]`) are a built-in form of restricted
access. `@restricted` is the user-definable form for ordinary struct types.

## 2. Surface

An **unnamed** restriction applies the allow-list to the type itself (no
parallel view name). Ordinary use sites see only the listed names. In a
function whose first parameter is that type / pointer, the body is unrestricted
(trusted method bodies). Aggregate / designated initialization may name any
field of the type being constructed (const-shaped: construction is open, later
use is not):

```c
@restricted on Box {
    r: len, bump;
};

static int box_bump(Box* b) { /* first arg → full Box */
    b->secret++;
    return ++b->len;
}

Box b = { .secret = 7, .len = 0 }; /* init OK */
/* b.secret; */                   /* ill-formed at ordinary use */
```

A named mode on a completed struct type lists names under use-kind groups:

```c
typedef struct Conn {
    CCSocket* sock;
    CCArena* arena;
    CCString out;
} Conn;

@restricted Encode on Conn {
    r: write, lit, simple, err, ok, pong, null, integer, bulk, array_*;
}

@restricted View on Slice {
    r: *;
}

@restricted Buf on Conn {
    r: write, flush;
    rw: out;
}
```

- `Encode` is the mode name. `Conn` is the base type.
- Groups are `r:`, `w:`, and `rw:`, each a comma-separated list of patterns
  ended by `;` or `}`. Absent groups are empty.
- Each pattern names a field of `Base` or a UFCS method on `Base*`, or a
  trailing-`*` glob (same rule as `cc_type_register` type patterns: literal
  prefix, optional final `*`; `Foo_*` also matches bare `Foo`). Unknown
  non-glob names are ill-formed at the mode declaration.
- **`r:`** — may use the name without storing through it: field load or
  UFCS call (`s->len`, `c->write(...)`).
- **`w:`** — may store through a field (`c->out = …`, `+=`, `++`, …).
- **`rw:`** — both use and store.
- Method names belong under `r:` (or `rw:`); a method under `w:` alone cannot
  be called (there is no store form for methods).
- Prefer methods over fields. A method whose first parameter is `Base*` sees
  the full object in its body; listing a field is only for call sites that
  must touch storage. Bare `r: *` on a type that mixes capabilities re-opens
  every field as a load — use narrow globs (`*_len`, `get_*`) when possible.
- A base type may declare several modes with distinct names.
- The mode declaration appears at file scope in the same translation unit
  that can see the base type's definition.

A bare comma-list with no group label is sugar for a single `r:` group
(legacy spelling).

Parameter and local sugar names the mode on the base type:

```c
static bool !>(CCError) exec_keys(DB* db, Cmd* c,
                                  @restricted(Encode) Conn* conn);
```

`@restricted(Encode) Conn` and the mangled type for that mode (§3) denote the
same type. Writing the mangled name is equivalent sugar-free spelling.

A typedef may alias the restricted type. Aliases are transparent for member
checks and UFCS (same allow-list as the mangled view):

```c
typedef @restricted(Encode) Conn* ConnEnc;
static bool !>(CCError) exec_keys(DB* db, Cmd* c, ConnEnc conn);
```

Mode definition and alias may be one declaration (C `typedef struct` shape):

```c
typedef @restricted Encode on Conn {
    r: write, lit, simple, err, ok, pong, null, integer, bulk, array_*;
} *ConnEnc;
```

`ConnEnc` is then `Conn_Restrict_Encode*`. Omit `*` before the alias for a
value-type alias of `Conn_Restrict_Encode`.

## 3. Mangle and type identity

`@restricted Mode on Base` introduces a distinct type whose name is
deterministic from the base and mode:

```text
Base_Restrict_Mode
```

Examples: `Conn_Restrict_Encode`, `Buf_Restrict_Read`.

- Language-owned: a user `typedef` or struct tag must not redefine that
  identifier. Doing so is ill-formed.
- Layout and representation of `Base_Restrict_Mode*` match `Base*` (same
  object, pointer retype). There is no extra storage and no vtable.
- The allow-list is part of the type's meaning for checking only. After
  lowering, a restricted pointer is an ordinary `Base*` (or equivalent);
  the mode name may remain as a typedef alias for readability in generated
  C, with no runtime tag.

### Unify or collide

| Situation | Result |
|-----------|--------|
| Same base, same mode, same (pattern, kind) set | Same type; redeclaration is OK (unifies) |
| Same base, same mode, different allow-list | Ill-formed (mangle collision with incompatible definition) |
| Same mode name on different bases | Different types (`Conn_Restrict_Encode` ≠ `Foo_Restrict_Encode`) |

Allow-list comparison is by set of `(pattern, kind)` pairs (order
irrelevant; duplicate patterns in one declaration are ill-formed).

### Glob matching

A name is allowed when any pattern matches it (OR). Matching uses the UFCS
type-register rule (trailing `*`). When several patterns match the same name
with different use-kinds, the longest literal prefix wins; a tie with
disagreeing kinds is ill-formed.

On owned types, prefer exact names. On open type-families (slices, and other
types extended by third-party UFCS / `@as`), globs are the membership
protocol: the mode owner publishes a pattern contract; extenders join by
naming into that family. A mode that listed every method would freeze the
day it shipped. Accidental membership (a mutating method named into a read
family) is a published-contract hazard — the same class as Go interface
method-name collisions. A diagnostic that expands which concrete names
currently match the patterns is required; it is the readable form of the
resolved contract.

### Type-family bases

When `Base` is a type family (slice `T[:]`, `CCSlice`, and similar), the
mode is keyed by `(mode-name, type-family)`, not by a concrete
instantiation. Instantiation does not fork the facet: `char[:]` and
`int[:]` share one `View` mode on the slice family. Implementations must
not key the mode on a single monomorph (e.g. only `char[:]`).

## 4. Coercion (const-shaped)

Restriction is one-way, like `const`:

- **Narrow (always, no cast):** `Base*` converts to `Base_Restrict_Mode*`
  (and to `@restricted(Mode) Base*`) implicitly wherever a pointer is
  passed or used as a UFCS receiver. No cast is required or written.
- **Widen (never):** `Base_Restrict_Mode*` does not convert to `Base*`.
  There is no cast form that restores full access. Discarding the
  restriction is ill-formed, not a warning.

`T*` → `const T*` is the model; `const T*` → `T*` is not offered for
restrictions. Value forms follow the same rule when structs are passed by
value; the primary use is pointers.

## 5. Member access and UFCS

On an expression of type `Base_Restrict_Mode` or `Base_Restrict_Mode*`:

- Field load requires a matching `r:` or `rw:` pattern.
- Field store requires a matching `w:` or `rw:` pattern.
- UFCS call requires a matching `r:` or `rw:` pattern.
- A miss does not fall through to the base's unlisted methods (no `@as`-style
  retry from restriction to full base).
- `@as` retry on the base, when applicable to a listed method, still applies
  for that method's resolution on the base; the restriction only filters which
  names may be attempted.

### Call edge

Allow-listed UFCS on a restricted receiver selects the same callee as on
`Base*`. The implementation may retype the receiver to `Base*` only inside
that lowering step so an existing `conn_write(Conn*, …)` body runs; that
retype is not a user-visible conversion and does not make `Base*` available
in the caller's scope. Declaring a function whose first parameter is
`Base_Restrict_Mode*` installs a UFCS method on the restricted type in the
usual way; such a function may also appear in the allow-list by its UFCS name.

A restricted pointer still cannot be passed to a `Base*` parameter (§4).
Callees that need full access take `Base*` and are invoked only from scopes
that already hold `Base*`.

## 6. Relation to `@as`

| | `@as` | `@restricted` |
|---|---|---|
| Direction | Widen: Outer gains Inner's methods through a field | Narrow: fewer fields and methods on the same object |
| Attachment | Field on the struct | Named mode on the type |
| Type identity | Outer and Inner remain distinct; path is a subobject | Mangled view type; same object as `Base*` |

`@as` and `@restricted` compose in the obvious way: a restricted receiver may
use a listed method that `@as`-resolves on the base; restriction never grants
unlisted names.

## 7. Worked example — Conn encode vs ship

```c
typedef struct Conn {
    CCSocket* sock;
    CCArena* arena;
    CCString out;
} Conn;

@restricted Encode on Conn {
    r: write, lit, simple, err, ok, pong, null, integer, bulk, array_*;
}

/* mangled type: Conn_Restrict_Encode */

static bool !>(CCError) conn_write(Conn* c, char[:] data);
static bool !>(CCError) conn_flush(Conn* c);
/* … other conn_* encode helpers on Conn* … */

static bool !>(CCError) exec_keys(DB* db, Cmd* c,
                                  @restricted(Encode) Conn* conn) {
    conn->array_len(c->argc - 1) !>;
    /* conn->flush(); */   /* ill-formed: flush not in Encode */
    /* conn->sock; */      /* ill-formed: sock not in Encode */
    /* conn->out = …; */   /* ill-formed: out not listed */
    return cc_ok(true);
}

static bool !>(CCError) execute(DB* db, Cmd* c,
                                @restricted(Encode) Conn* conn) {
    /* encode only — ship stays on Conn* in the caller */
    …
}
```

Under a shard hold, handlers receive only encode access. Ship (`flush` /
socket) stays on the outer `Conn*` after release. Flush-under-hold does not
parse at the restricted receiver; neither does reaching `sock` directly.

## 7b. Worked example — slice family (unnamed, first-arg exception)

The slice case uses the **unnamed** form: the allow-list is the ordinary
surface of the type family. No parallel view name.

```c
@restricted on CCSlice {
    r: *;
};
```

Installed for the slice family by the lowerer (`CCSlice`, markers, and
`CCSlice_T` share one unnamed facet). `r: *` is observe + call: field
loads and UFCS are open; field stores (`s.len = …`, `s.ptr = …`) are
ill-formed. Narrower published families (`as_*`, `get_*`, …) remain the
membership protocol when a mode wants a smaller read surface; the default
slice contract is “no field writes.”
- **First-parameter exception:** in a function whose first parameter is
  `CCSlice` / `CCSlice*` (or the concrete `T[:]` receiver), the body sees
  the full object — trusted method bodies (`cc_slice_*`, user UFCS with
  slice first). That is how mutators and allocators keep writing fields
  without listing them on the public surface.
- Keyed by `(unnamed-default, slice type-family)`; instantiation does not
  fork the facet (`char[:]` and `int[:]` share it).
- Bare `r: *` is not this contract — it re-opens every field load.

Named modes (`@restricted Encode on Conn`) remain for multiple facets on
one owned type. Unnamed + first-arg exception is the default for open
families such as slices.

## 8. Ill-formed cases

- Mode entry that is neither a field of `Base` nor a UFCS method name on
  `Base*` (non-glob patterns).
- Duplicate patterns in one allow-list.
- Redeclaring `Base_Restrict_Mode` with a different allow-list set.
- User definition of the mangled identifier.
- Member or UFCS use of an unmatched name on a restricted receiver.
- Field store without a matching `w:` / `rw:` pattern.
- Field load or UFCS call without a matching `r:` / `rw:` pattern.
- Same name matched by equal-score patterns with disagreeing use-kinds.
- Any conversion or cast restricted → full `Base*` (widen).
- Requiring a cast to narrow `Base*` → restricted (narrowing is always
  implicit).
- `@restricted(Mode)` naming a mode not declared on that base.

## 9. Non-goals

- Region or effect denial (operations forbidden for every object in a scope).
  Per-object specimens such as read-only `len` with open method extension
  are expressed here (`r: len` plus globs), not as effects.
- Changing `@as` semantics.
- Extra allocation, reference counting, or dynamic dispatch for restrictions.
