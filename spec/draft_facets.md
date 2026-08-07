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

A named mode on a completed struct type lists the fields and UFCS method names
that remain available under that mode:

```c
typedef struct Conn {
    CCSocket* sock;
    CCArena* arena;
    CCString out;
} Conn;

@restricted Encode on Conn {
    arena, out,
    write, lit, simple, err, ok, pong, null, integer, bulk, array_len
}
```

- `Encode` is the mode name. `Conn` is the base type.
- Each entry is either a field of `Conn` or a UFCS method name resolvable on
  `Conn*` (bare-name or family tier). Unknown names are ill-formed at the
  mode declaration.
- The list is closed: under the restriction, only listed fields and methods
  are available. Unlisted members and methods do not resolve.
- A base type may declare several modes with distinct names.
- The mode declaration appears at file scope in the same translation unit
  that can see the base type's definition.

Parameter and local sugar names the mode on the base type:

```c
static bool !>(CCError) exec_keys(DB* db, Cmd* c,
                                  @restricted(Encode) Conn* conn);
```

`@restricted(Encode) Conn` and the mangled type for that mode (§3) denote the
same type. Writing the mangled name is equivalent sugar-free spelling.

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
| Same base, same mode, same allow-list set | Same type; redeclaration is OK (unifies) |
| Same base, same mode, different allow-list | Ill-formed (mangle collision with incompatible definition) |
| Same mode name on different bases | Different types (`Conn_Restrict_Encode` ≠ `Foo_Restrict_Encode`) |

Allow-list comparison is by set of names (order irrelevant; duplicates in one
declaration are ill-formed).

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

- Field access succeeds only for fields named in the mode's allow-list.
  `conn->sock` is ill-formed when `sock` is not listed.
- UFCS resolves only method names listed in the allow-list.
  `conn->flush()` is ill-formed when `flush` is not listed.
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
    arena, out,
    write, lit, simple, err, ok, pong, null, integer, bulk, array_len
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
    return cc_ok(true);
}

static bool !>(CCError) execute(DB* db, Cmd* c, Conn* conn) {
    CCShardHold hold = /* acquire */ …;
    @defer hold.release();
    exec_keys(db, c, conn) !>;   /* Conn* → Conn_Restrict_Encode* */
    return conn->flush();        /* full Conn* still in scope here */
}
```

Under a shard hold, handlers receive only encode access. Ship (`flush` /
socket) stays on the outer `Conn*` after release. Flush-under-hold does not
parse at the restricted receiver; neither does reaching `sock` directly.

## 8. Ill-formed cases

- Mode entry that is neither a field of `Base` nor a UFCS method name on
  `Base*`.
- Duplicate names in one allow-list.
- Redeclaring `Base_Restrict_Mode` with a different allow-list set.
- User definition of the mangled identifier.
- Member or UFCS use of an unlisted name on a restricted receiver.
- Any conversion or cast restricted → full `Base*` (widen).
- Requiring a cast to narrow `Base*` → restricted (narrowing is always
  implicit).
- `@restricted(Mode)` naming a mode not declared on that base.

## 9. Non-goals

- Region or effect denial (operations forbidden for every object in a scope).
- Changing `@as` semantics.
- Extra allocation, reference counting, or dynamic dispatch for restrictions.
