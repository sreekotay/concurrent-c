# Type views (`@typeview`)

Status: draft — implemented in shadow_lower (`tests/restricted_*`,
`tests/typeview_as_ufcs_smoke.ccs`). Viewed faces (`(Mode)field` in `as:`)
— not implemented.

## 1. Notion

A type view is named or unnamed access policy on the same object. It
generalizes `const`: the pointer identity is unchanged; the set of available
fields and UFCS methods may shrink; narrowing (full → view) is always
implicit with no cast; widening (view → full) never occurs. Zero runtime
cost — a type-system view, not a second object. Views are erased in lowering;
they do not exist at run time.

The same form also declares **is-a faces** (`as:`): field names that the
outer type forwards through for UFCS retry, arg coerce, and `@errhandler`
projection (see `draft_as.md`).

Channel split handles (`T[~ >]` / `T[~ <]`) are a built-in form of restricted
access. `@typeview` is the user-definable form for ordinary struct types.

## 2. Surface

Preferred spelling is `@typeview`.

An **unnamed** view applies to the type itself (no parallel view name).
Ordinary use sites see only allow-listed names when `r:`/`w:`/`rw:` are
present. An `as:`-only unnamed view registers faces without locking the
allow-list. In a function whose first parameter is that type / pointer, the
body is unrestricted (trusted method bodies). Aggregate / designated
initialization may name any field of the type being constructed
(const-shaped: construction is open, later use is not):

```c
@typeview on Box {
    r: len, bump;
};

static int box_bump(Box* b) { /* first arg → full Box */
    b->secret++;
    return ++b->len;
}

Box b = { .secret = 7, .len = 0 }; /* init OK */
/* b.secret; */                   /* ill-formed at ordinary use */
```

Is-a faces on the type:

```c
typedef struct CCTempFile {
    CCFile file;
    CCSlice path;
    int owns;
} CCTempFile;

@typeview on CCTempFile {
    as: file;
};
```

A viewed face exposes a field through one of its type's named modes:

```c
@typeview Region on CCArena {
    r: alloc, remaining, adopt, attach, create_*;
};

@typeview on CCNursery {
    as: (Region)n;        /* n.alloc(...) projects Region through the host;
                             n.reset() is ill-formed — not in Region */
};
```

A named mode on a completed struct type lists names under use-kind groups:

```c
typedef struct Conn {
    CCSocket* sock;
    CCArena arena;
    CCString out;
} Conn;

@typeview Encode on Conn {
    r: write, lit, simple, err, ok, pong, null, integer, bulk, array_*;
}

@typeview View on Slice {
    r: *;
}

@typeview Buf on Conn {
    r: write, flush;
    rw: out;
}
```

- `Encode` is the mode name. `Conn` is the base type.
- Groups are `r:`, `w:`, `rw:`, and `as:`, each a comma-separated list of
  patterns ended by `;` or `}`. Absent groups are empty.
- Each `r:`/`w:`/`rw:` pattern names a field of `Base` or a UFCS method on
  `Base*`, or a name glob: trailing `foo*` / `get_*` (prefix match) or leading
  `*suffix` / `*_len` (suffix match). Bare `*` matches every name. A leading
  `^` subtracts: `^p`, `^get_secret`. Allow-set first, then subtract; a
  matching deny wins. A group that contains only denies implies `*`
  (`r:^p` is `r: *, ^p`). `^*` is ill-formed. `p` and `^p` in the same
  group are ill-formed. `as:` cannot use `^`. Type-family subjects use the
  same trailing-`*` rule as `@typehooks`. Unknown non-glob names are
  ill-formed at the mode declaration.
- **`r:`** — may use the name without storing through it: field load or
  UFCS call (`s->len`, `c->write(...)`).
- **`w:`** — may store through a field (`c->out = …`, `+=`, `++`, …).
- **`rw:`** — both use and store.
- **`as:`** — is-a faces (at most one path per target type). Not an
  allow-list group. Each pattern is `field` or `(Mode)field`. `as:` is a
  projection: when the outer misses, retry through this path on that
  face — UFCS first, then a field load or store that is not a member of
  the outer (`xs.len` on `CCSlice_int` → `xs.base.len`). The plain form
  exposes a value embed's full surface, or a
  single-pointer hop (`as: p` on `CCBox_*` / a teaching alias of
  `CCBox::[H]` — UFCS miss retries on the host). The
  parenthesized form takes a named mode of the landing — `(Mode)` names
  the face, not the hop's static type. A pointer field is a hop
  (`as: (Region)n` on `CCNursery` projects Region through the host; the
  host is not an arena). Exactly one mode may appear. The name resolves
  first as a mode declared on the landing type; otherwise it must be a
  type name denoting a **value** view of that type (`Base_Restrict_Mode`
  or a value alias). A pointer alias is ill-formed as the landing — the
  landing is an object. The cast spelling exists only in `as:` patterns;
  expression-site narrowing stays implicit (§4). Explicit member access
  through the field (`t.field.name`) is unaffected by the mode.
- The subject `Base` may be a trailing-`*` type-family glob (`CCSlice_*`),
  same match/score rule as allow-list patterns and `@typehooks`.
  Narrowest matching view wins; equal-score conflicts are ill-formed.
  Unnamed `@typeview on Pat*` is the ordinary surface of each match.
  Named `@typeview Mode on Pat*` is one mode for the family; the use site
  is `@typeview(Mode) Concrete*` and mangles to `Concrete_Restrict_Mode`.
  A `typedef` alias of a glob subject is ill-formed (one name cannot denote
  the family). Face fields must exist on each concrete match; types that
  match the glob but lack the field are skipped.
- Method names belong under `r:` (or `rw:`); a method under `w:` alone cannot
  be called (there is no store form for methods).
- Prefer methods over fields. A method whose first parameter is `Base*` sees
  the full object in its body; listing a field is only for call sites that
  must touch storage. Bare `r: *` on a type that mixes capabilities re-opens
  every field as a load — use `^name` when the surface is open except one
  field, or narrow globs (`*_len`, `get_*`) when the surface is a subset.
- A base type may declare several modes with distinct names.
- The mode declaration appears at file scope in the same translation unit
  that can see the base type's definition.

A bare comma-list with no group label is sugar for a single `r:` group
(legacy spelling).

Parameter and local sugar names the mode on the base type:

```c
static bool !>(CCError) exec_keys(DB* db, Cmd* c,
                                  @typeview(Encode) Conn* conn);
```

`@typeview(Encode) Conn` / `@typeview(Encode) Conn` and the mangled type
for that mode (§3) denote the same type. Writing the mangled name is
equivalent sugar-free spelling.

A typedef may alias the restricted type. Aliases are transparent for member
checks and UFCS (same allow-list as the mangled view):

```c
typedef @typeview(Encode) Conn* ConnEnc;
static bool !>(CCError) exec_keys(DB* db, Cmd* c, ConnEnc conn);
```

Mode definition and alias may be one declaration (C `typedef struct` shape):

```c
typedef @typeview Encode on Conn {
    r: write, lit, simple, err, ok, pong, null, integer, bulk, array_*;
} *ConnEnc;
```

`ConnEnc` is then `Conn_Restrict_Encode*`. Omit `*` before the alias for a
value-type alias of `Conn_Restrict_Encode`.

## 3. Mangle and type identity

`@typeview Mode on Base` introduces a distinct type whose name is
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

On owned types, prefer exact names. Exact positive and deny patterns on a
concrete type are checked at registration: a typo that matches no field or
UFCS method is ill-formed (deny typos that subtract nothing are not silent).
On open type-families (slices, and other
types extended by third-party UFCS / `as:` faces), globs are the membership
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
  (and to `@typeview(Mode) Base*`) implicitly wherever a pointer is
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
- A miss does not fall through to the base's unlisted methods (no `as:`-style
  retry from restriction to full base).
- `as:` retry on the base, when applicable to a listed method, still applies
  for that method's resolution on the base; the restriction only filters which
  names may be attempted.
- An explicit field selection (`d->tree.write_fd(fd)`) is member access
  plus UFCS on the field's type. The outer view's allow-list does not
  apply; `write_fd` need not be listed on the outer mode. `d->len()`
  with no peel is still the outer.

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

## 6. Relation to `as:` faces

| | `as:` face | named `@typeview` |
|---|---|---|
| Direction | Widen: Outer gains Inner's methods through a field | Narrow: fewer fields and methods on the same object |
| Attachment | Field name listed in `@typeview on Outer { as: … }` | Named mode on the type |
| Type identity | Outer and Inner remain distinct; path is a subobject | Mangled view type; same object as `Base*` |

A view receiver may use a listed method that `as:`-resolves on the base;
restriction never grants unlisted names.

## 7. Worked example — Conn encode vs ship

```c
typedef struct Conn {
    CCSocket* sock;
    CCArena arena;
    CCString out;
} Conn;

@typeview Encode on Conn {
    r: write, lit, simple, err, ok, pong, null, integer, bulk, array_*;
}

/* mangled type: Conn_Restrict_Encode */

static bool !>(CCError) conn_write(Conn* c, char[:] data);
static bool !>(CCError) conn_flush(Conn* c);
/* … other conn_* encode helpers on Conn* … */

static bool !>(CCError) exec_keys(DB* db, Cmd* c,
                                  @typeview(Encode) Conn* conn) {
    conn->array_len(c->argc - 1) !>;
    /* conn->flush(); */   /* ill-formed: flush not in Encode */
    /* conn->sock; */      /* ill-formed: sock not in Encode */
    /* conn->out = …; */   /* ill-formed: out not listed */
    return cc_ok(true);
}

static bool !>(CCError) execute(DB* db, Cmd* c,
                                @typeview(Encode) Conn* conn) {
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
@typeview on CCSlice {
    r: *;
};
```

Installed for the slice family by the lowerer (`CCSlice`, markers, and
`CCSlice_T` share one unnamed facet). Ordinary sites may read `.ptr` /
`.len` / `.id`; field stores (`s.len = …`) are ill-formed. Raw
`s.ptr[i]` is the expressible Gap.
`@typehooks` `.len` / `.access` own extent and walk policy — the same
user story (do not lie about length; do not index past it), two
mechanisms. Narrower published families (`as_*`, `get_*`, …) remain the
membership protocol when a mode wants a smaller read surface; the default
slice contract is “no field writes.”
- **First-parameter exception:** in a function whose first parameter is
  `CCSlice` / `CCSlice*` (or the concrete `T[:]` receiver), the body sees
  the full object — trusted method bodies (`cc_slice_*`, user UFCS with
  slice first). That is how mutators and allocators keep writing fields
  without listing them on the public surface.
- Keyed by `(unnamed-default, slice type-family)`; instantiation does not
  fork the facet (`char[:]` and `int[:]` share it).

Named modes (`@typeview Encode on Conn`) remain for multiple facets on
one owned type. Unnamed + first-arg exception is the default for open
families such as slices. Prefer narrow globs on types that mix unrelated
capabilities (see Conn / Encode in §2); the slice family's `r: *` is the
default open-observe, no-field-write contract.

The named-pointer family uses the opposite contract: methods, not the
host field.

```c
@typeview on CCBox_* {
    r: ^p;
};
```

Ordinary sites may call any method (`is_live`, `host`, user UFCS) and
must not load `.p`. Factory aliases (`CCArena`, `CCNursery`,
`CCExclusive`) are other subjects and do not match `CCBox_*`.
First-parameter bodies stay trusted. Designated init may still name `.p`.

`CCString` hides the SSO union (`.data` is not a pointer when the
bytes are inline). `.len` / `.cap` are readable; field stores are
ill-formed. The view is `as_slice()`; a C string is `cstr(arena)`.

```c
@typeview on CCString {
    r: ^data, ^inline_buf, ^_inline_word;
};
```

## 8. Ill-formed cases

- Mode entry that is neither a field of `Base` nor a UFCS method name on
  `Base*` (non-glob patterns).
- Duplicate patterns in one allow-list.
- `p` and `^p` in the same group, or `^*`.
- `^` on an `as:` face.
- Redeclaring `Base_Restrict_Mode` with a different allow-list set.
- User definition of the mangled identifier.
- Member or UFCS use of an unmatched name on a restricted receiver.
- Field store without a matching `w:` / `rw:` pattern.
- Field load or UFCS call without a matching `r:` / `rw:` pattern.
- Same name matched by equal-score patterns with disagreeing use-kinds.
- Any conversion or cast restricted → full `Base*` (widen).
- Requiring a cast to narrow `Base*` → restricted (narrowing is always
  implicit).
- `@typeview(Mode)` naming a mode not declared on that base.
- `(Mode)field` in an `as:` group where the name is not a mode of the
  field's type and not a value view type of the field's type; where it
  denotes a view of a different base; where it is a pointer alias; or
  where more than one mode is listed.

## 9. Non-goals

- Region or effect denial (operations forbidden for every object in a scope).
  Per-object specimens such as read-only `len` with open method extension
  are expressed here (`r: len` plus globs), not as effects.
- Changing `as:` face semantics.
- Extra allocation, reference counting, or dynamic dispatch for restrictions.
