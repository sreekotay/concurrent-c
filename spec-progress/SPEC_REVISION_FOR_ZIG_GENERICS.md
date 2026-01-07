# Spec Revision: Adopt Zig-Style Comptime Generics

**Current State:** Spec designed for C++ static generics (Vec<T>)  
**New State:** Spec should adopt Zig-style comptime generics (Vec(T))  
**Impact:** Several spec sections need updates

---

## Changes Needed

### 1. Language Constructs Table (Quick Reference)

**Current:**
```
Library Functions (6): cc_ok, cc_err, cc_move, cc_cancel, etc.
```

**New:** Add comptime functions as first-class construct

```
### Comptime Functions (NEW)

| Form | Purpose | Example |
|------|---------|---------|
| `fn Name(comptime T) { return struct {...}; }` | Generate types at comptime | `fn Vec(comptime T) { return struct { T* items; }; }` |
| `typedef Name(Type) Alias;` | Instantiate comptime function | `typedef Vec(int) IntVec;` |
```

---

### 2. @comptime Section (Expand)

**Current:**
```
@comptime if (condition) { ... }
```

**New:** Add comptime functions + blocks

```
### Comptime Evaluation

CC supports compile-time computation in two forms:

#### @comptime if - Conditional Compilation

```c
const int VERSION = 2;

@comptime if (VERSION > 1) {
    void feature_v2() { ... }
}
```

Rules:
- Condition must be evaluable at comptime (consts, literals, simple expressions)
- Block either fully included or fully excluded in output
- Uses TCC JIT for evaluation

#### Comptime Functions - Type Generation

```c
fn Vec(comptime T) {
    return struct {
        T* items;
        usize len;
        usize cap;
    };
}

// Usage:
typedef Vec(int) IntVec;
typedef Vec(Task*) TaskVec;

IntVec ints;
TaskVec tasks;
```

Rules:
- Function declaration with `comptime` parameters
- Function body executes at compile time (comptime block)
- `return` in comptime context returns a type
- Typedef captures the generated type
- Each unique instantiation creates unique type
- No code bloat (same type = same code)
```

---

### 3. Type System Section

**Current:** Doesn't really cover generics

**New:** Add section on generic types

```
### Generic Types

CC supports generic type generation via comptime functions. Any function that returns a type becomes a "generic type factory."

#### Standard Generic Types

The stdlib provides common generics:

```c
// Vector/List
fn Vec(comptime T) type { ... }
typedef Vec(int) IntVec;

// Hash Map
fn Map(comptime K, comptime V) type { ... }
typedef Map(char[:], Task*) TaskMap;

// Optional/Result variants
fn Optional(comptime T) type { ... }
fn Result(comptime T, comptime E) type { ... }
```

#### Writing Your Own Generics

```c
fn Stack(comptime T) {
    return struct {
        items: T*,
        top: usize,
        capacity: usize,
        
        fn push(self: *@This(), value: T) void { ... }
        fn pop(self: *@This()) T? { ... }
    };
}

typedef Stack(int) IntStack;
```

#### Generics with Multiple Parameters

```c
fn Pair(comptime A, comptime B) {
    return struct {
        first: A,
        second: B,
    };
}

typedef Pair(int, char[:]) IntStringPair;
```

#### Generics with Comptime Values

```c
fn Array(comptime T, comptime size: usize) {
    return struct {
        data: T[size],
        len: usize,
    };
}

typedef Array(int, 100) IntArray100;
typedef Array(int, 256) IntArray256;
```
```

---

### 4. Code Examples Throughout

**Current examples use C++ syntax:**
```c
Vec<int> v;
Map<Task*, int> m;
```

**New examples use Zig syntax:**
```c
typedef Vec(int) IntVec;
IntVec v;

typedef Map(Task*, int) TaskMap;
TaskMap m;
```

**Locations to update:**
- Section 1 examples
- Section 8 (Standard Library) examples
- Appendix examples

---

### 5. Stdlib Type Signatures

**Current (if we even specified):**
```c
Vec<T> vec_new<T>(Arena *a);
Map<K, V> map_new<K, V>(Arena *a);
```

**New:**
```c
// Vec(T) - Generic vector type factory
fn Vec(comptime T) {
    return struct {
        T* items;
        usize len;
        usize cap;
        Arena* arena;
        
        fn push(self: *@This(), item: T) !void { ... }
        fn pop(self: *@This()) T? { ... }
        fn get(self: *@This(), index: usize) T? { ... }
    };
}

// Usage:
typedef Vec(int) IntVec;
IntVec ints;
ints.push(42);

// Map(K, V) - Generic hash map type factory
fn Map(comptime K, comptime V) {
    return struct {
        // Hash map implementation
        K* keys;
        V* values;
        // ...
        
        fn insert(self: *@This(), key: K, value: V) !void { ... }
        fn get(self: *@This(), key: K) V? { ... }
    };
}

// Usage:
typedef Map(int, Task*) TaskMap;
TaskMap tasks;
tasks.insert(1, task);
```

---

### 6. Language Constructs Count

**Current:** ~40 constructs

**New:** ~45 constructs (add generics pattern)

Actually still ~40-45 because:
- Comptime functions = same as regular functions (already counted)
- Typedef = same as typedef (already counted)
- Just new semantic meaning

---

## Affected Sections in Language Spec

1. **Quick Reference Table** — Add comptime functions row
2. **Comptime Section** — Expand significantly
3. **Type System** — Add generics subsection
4. **Examples** — Update Vec<T> to Vec(T) throughout
5. **Stdlib Integration** — Update Vec, Map examples

---

## Affected Sections in Stdlib Spec

1. **Vec/List** — Define as comptime function
2. **Map** — Define as comptime function
3. **Optional/Result** — Define as comptime generics
4. **Examples** — Update throughout
5. **Usage patterns** — Show typedef + use pattern

---

## Spec Changes Summary

| Section | Change | Impact |
|---------|--------|--------|
| Quick Reference | Add comptime functions | Minor |
| @comptime section | Expand significantly | Medium |
| Type System | Add generics subsection | Medium |
| Examples | Vec<T> → Vec(T) | Extensive |
| Stdlib | Redefine Vec, Map as comptime | Major |
| Language Constructs | Still ~40-45 | None |

---

## Updated Language Constructs (Revised Count)

### Core Keywords (7) - unchanged
```
await, try, catch, spawn, defer, unsafe, comptime
```

### Contextual Keywords (5) - unchanged
```
async, arena, lock, noblock, closing
```

### @ Statements (11) - unchanged
```
@async, @latency_sensitive, @scoped, @nursery, @arena, @lock, @match, @defer, @for await, @comptime, @nursery closing
```

### Type Constructors (4) - unchanged
```
T!E, T?, char[:], char[:!]
```

### Expression Forms (4) - unchanged
```
await expr, try expr, spawn (expr), result conditional
```

### Block Forms (2) - unchanged
```
cc_with_deadline(d) { }, try { } catch { }
```

### Comptime Constructs (NEW - 3)
```
comptime fn Name(comptime Param) { }
@comptime if (condition) { }
typedef Name(Type) Alias;
```

### Library Functions (6) - unchanged
```
cc_ok, cc_err, cc_move, cc_cancel, cc_is_cancelled, cc_with_deadline
```

**New Total:** ~43 constructs (basically same)

---

## Quick Wins (Easiest Changes)

1. **Update Quick Reference table** — Just add comptime functions row
2. **Update @comptime section** — Expand explanation + examples
3. **Update all Vec<T> examples** — Search/replace to Vec(T)
4. **Update all Map<K,V> examples** — Search/replace to Map(K,V)

---

## Bigger Changes (More Work)

1. **Type System section** — Add substantial "Generics" subsection
2. **Stdlib spec** — Rewrite Vec, Map definitions as comptime functions
3. **Architecture examples** — Show comptime type factory pattern

---

## Timeline for Spec Updates

If we're proceeding with Zig-style generics in Phase 1:

**Update specs:** 1-2 hours
- Quick Reference: 15 min
- @comptime section: 30 min
- Type System section: 30 min
- Examples throughout: 30-45 min
- Stdlib specs: 30 min

**Validation:** 30 min
- Consistency checks
- Example validation
- Cross-reference validation

**Total:** 2-3 hours

---

## Do We Update Specs Before or After TCC Analysis?

**Recommendation: After TCC analysis**

Why:
1. TCC analysis validates implementation feasibility
2. May discover architectural constraints
3. Better to spec what we actually can build
4. Small timing difference (TCC reading ~2-4 hours, spec update ~2-3 hours)

**Sequence:**
1. Read TCC source
2. Validate comptime function generation is feasible
3. Update specs to reflect Zig-style approach
4. Proceed to implementation

**If TCC analysis shows it's harder than expected:**
- Can revert to simpler approach (just @comptime if)
- Specs would guide us back

---

## Spec Consistency Check

After updating, verify:
- ✅ All examples use Vec(T) consistently
- ✅ All examples use Map(K,V) consistently
- ✅ Comptime functions documented clearly
- ✅ @comptime if still documented clearly
- ✅ Difference from C++ generics explained
- ✅ Difference from Zig generics explained (we're simpler)
- ✅ All code blocks are valid CC syntax

---

## Summary

**Changes needed:**
1. Quick Reference: Add comptime functions
2. @comptime section: Expand with function generics
3. Type System: Add Generics subsection
4. Examples: Vec<T> → Vec(T), Map<K,V> → Map(K,V) everywhere
5. Stdlib spec: Redefine collections as comptime functions

**Effort:** 2-3 hours  
**Risk:** Low (mostly documentation updates)  
**Benefit:** Specs now align with Zig-style implementation approach

**Proceed:**
1. Finish TCC analysis ✓ (you'll do this next)
2. Validate approach is feasible
3. Update specs
4. Begin implementation

---

