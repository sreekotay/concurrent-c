# Hostile host contract (frozen)

Normative for every Track A implementation. **Do not redesign this API
into something more CC-shaped or more Rust-shaped.** Names below are
conceptual; the ABI is [`hostile_contract.h`](hostile_contract.h).

## Operations

```text
retain(value) -> Claim
release(Claim)

borrow(Claim) -> transient Value   # usable only within the established scope

weak(value) -> Weak
upgrade(Weak) -> Result<Claim>

register_callback(value) -> Registration
unregister(Registration)
invoke_registered(Registration)   # host turn: call the retained function
```

## Semantics

### Persistent claim

> The host currently requires this JS value to remain live.

Releasing a claim means only:

> This host claim no longer exists.

It does **not** mean “destroy the JS object.” Multiple claims on the same
identity are independent; the value may remain live while any claim (or
other VM root) remains.

### Borrow

A borrowed value may only be used during the scope established by the
host API. Using a borrow after the scope ends is a protocol violation
(implementation may trap or return `HOSTILE_ERR_STALE_BORROW`).

### Weak

A weak handle identifies an object if that object remains live but
contributes **no** lifetime claim. Upgrade either produces a valid new
claim or reports `HOSTILE_ERR_EXPIRED`.

### Callback registration

A registration creates a persistent claim for as long as the
registration exists. Invoking after unregister is `HOSTILE_ERR_EXPIRED`.
Invoking after realm teardown is `HOSTILE_ERR_STALE_REALM`.

## Distinctions

```text
identity
≠ access
≠ temporary borrow
≠ lifetime claim
≠ weak observation
≠ destruction authority
```

## Error codes

See `hostile_contract.h`. Implementations map these to Result / Rust
`Result` without inventing silent success.
