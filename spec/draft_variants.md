# `@variant` Tagged Unions

Status: implemented

## Declaration and default layout

`@variant` declares a value-type tagged union:

```c
@variant RedisValue {
    str: CCString;
    num: int64_t;
};

@variant Signal {
    hup:  void;
    term: void;
    info: int;
};
```

An arm name is a non-keyword C identifier. Each arm has one type; a multi-field
payload uses a struct. `void` arms carry no payload. Recursive arms go through a
pointer or slice rather than containing the variant by value.

The default lowering is an honest tagged C union:

```c
typedef enum { RedisValue_str, RedisValue_num } RedisValueKind;
typedef struct RedisValue {
    RedisValueKind kind;
    union {
        CCString str;
        int64_t num;
    } u;
} RedisValue;
```

This is the same default enum-and-union layout used by schema `one of`.
Default-layout variants and schema `one of` share that layout and the same
protected consumption surface (construction, dominated projection, raw `.u`
ban); see `spec/cc_serdes.md`. They remain distinct families: schemas own
wire faces; variants own destructor/transition and packing.
`sizeof(Name)` includes the tag and the largest arm. Construction, projection,
switching, and transition do not allocate.

## Construction and transition

A designated initializer names exactly one arm. The compiler fills the tag and
lowered union path:

```c
RedisValue a = { .num = 42 };
RedisValue b = { .str = cc_string_new() };
RedisValue c = (RedisValue){ .num = 7 };
```

Naming multiple arms, an unknown arm, or a payload of the wrong type is a
compile-time error. A `void` arm uses an empty initializer:

```c
Signal s = { .hup = {} };
```

On the default layout, `{ .kind = Signal_hup }` is also a valid tag-only
initializer for a payload-less arm.

Braced assignment is defined for variant lvalues and performs an arm
transition:

```c
a = { .num = 43 };
*cell = (RedisValue){ .str = value };
```

The old active arm is destroyed before the new arm and tag are installed.
Assignment to a non-variant requires an ordinary C compound literal.

## Tag and projection

`value.kind` has the generated `NameKind` type and is read-only after
initialization. Tags change through construction or whole-variant transition,
not member assignment.

A bare `.arm` designator resolves from an expected variant-kind type:

```c
if (value.kind == .num) { /* ... */ }
RedisValueKind k = .str;
```

The generated `Name_arm` enum constants remain valid:

```c
if (value.kind == RedisValue_num) { /* ... */ }
```

Arm projection uses `value.arm` or `pointer->arm`. It is legal only when the
active arm is protected by one of:

- the corresponding case of a checked variant switch;
- a directly enclosing `if (value.kind == .arm)` or
  `if (value.kind == Name_arm)` in the same block;
- a `!>` handler, which runs when the arm is inactive and diverges;
- a `?>` fallback, which supplies a value of the arm's type when inactive.

```c
if (cell->kind == .num) {
    cell->num += delta;
}

int64_t n = cell->num ?> 0;
int64_t required = cell->num !> { return cc_err(error); };
```

The domination check is syntactic. It does not infer protection from preceding
early returns, boolean chains, or general data flow. Unprotected projection is a
compile-time error. User source cannot reach into the lowered `.u` union
directly.

## Checked switch

`switch` accepts a variant value or pointer as its subject. `case .arm:` labels
resolve from the subject type, and each case protects projection of its arm:

```c
switch (value) {
    case .str:
        use_string(value.str);
        break;
    case .num:
        use_number(value.num);
        break;
}
```

Every arm must appear unless the switch has a `default:`. Missing or unknown
arms are compile-time errors. The default-layout interop spelling
`switch (value.kind)` with `case Name_arm:` labels is checked the same way.

`@match` is not a variant branching construct. The keyword remains reserved;
variant branching uses protected projection or checked `switch`.

No constructor functions are generated. Designated initializers select an arm,
and the generated `Name_arm` identifiers remain enum constants.

## Destruction and moves

If an arm type has a registered destructor, the generated variant drop helper
switches on the active tag and destroys only that arm. Destructor-bearing local
variants are dropped on normal scope exit and early return.

A whole-variant transition drops the old active arm exactly once before
installing the replacement. A same-arm transition follows the same drop path.
If the old arm has no registered destructor, that path is a no-op.
Moving an arm with `cc_move` follows the ordinary move rules: the moved source
arm is dead, and the variant must be replaced before any later read.

## Results

Results and variants are orthogonal. `T !>(E)` remains the function error
channel, while `@variant` represents ordinary data alternatives:

```c
RedisValue !>(CCError) load_value(void);

RedisValue value = load_value() !>;
switch (value) {
    case .str: /* ... */ break;
    case .num: /* ... */ break;
}
```

There is no variant-propagation operator. The `!>` suffix on a variant
projection handles an inactive arm; the `!>` on a Result retains its Result
semantics.

## Packed layout

`@variant(packed)` accepts at most two arms and opts the variant into
compiler-proved niche packing:

```c
@variant(packed) OptionalPtr {
    some: int*;
    none: void;
};
```

The packed representation uses one sentinel distinction: a bit pattern that a
donor arm cannot hold distinguishes it from the other arm. Three or more arms
are a compile-time error because one sentinel cannot encode them. Niches are
either inferred from pointer null/alignment properties and payload-less arms,
or declared by a type's registered niche descriptor. Payload bits are not
stolen.

Packing is accepted only when every arm size is known and a lossless niche plan
exists. The compiler rejects unsupported arm counts or an unprovable layout;
it never silently falls back to the default representation.

Construction, braced assignment, read-only `.kind`, protected projection,
checked subject switch, destruction, and transition have the same semantics as
the default layout. `.kind` is decoded by generated accessors. Packed variants
expose neither a writable tag field nor a raw union. The shared layout promise
with schema `one of` applies only to the default (unpacked) representation;
packed niche layout is not schema-compatible.
