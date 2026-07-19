//! Where Rust wins: the same shape as race_pointer_alias.ccs.
//!
//! Safe Rust refuses to share `&mut i32` (or `*mut i32` without `unsafe`)
//! across threads. The Send/Sync + borrow checker close the pointer-alias
//! hole that CC's capture mutation check leaves open.
//!
//! Run (expect compile failure on the shared `&mut`):
//!   rustc examples/compare_rust_data_race/race_pointer_alias.rs
//!
//! The `atomic_ok` module below is the proven fix — same idea as
//! CCAtomic::[int] in Concurrent-C.

use std::thread;

const N_TASKS: usize = 8;
const N_ITERS: usize = 100_000;

fn main() {
    // --- rejected in safe Rust (uncomment to see the error) ---
    // error[E0499]: cannot borrow `counter` as mutable more than once at a time
    // and/or cannot be sent between threads safely
    /*
    let mut counter = 0i32;
    let p: *mut i32 = &mut counter;
    let mut handles = vec![];
    for _ in 0..N_TASKS {
        // Sharing a raw mutable pointer across threads requires `unsafe`
        // and is still a data race if you actually mutate without sync.
        handles.push(thread::spawn(move || {
            for _ in 0..N_ITERS {
                unsafe { *p += 1; }
            }
        }));
    }
    for h in handles { h.join().unwrap(); }
    */

    // Even the "looks like a shared int" version fails without Sync:
    //   let mut counter = 0;
    //   thread::spawn(|| { counter += 1; }); // E0373 / borrow

    println!("safe Rust: shared mutable alias across tasks does not compile");
    println!("see atomic_ok() for the proven pattern");
    atomic_ok();
}

fn atomic_ok() {
    use std::sync::atomic::{AtomicI32, Ordering};
    use std::sync::Arc;

    let counter = Arc::new(AtomicI32::new(0));
    let mut handles = vec![];
    for _ in 0..N_TASKS {
        let c = Arc::clone(&counter);
        handles.push(thread::spawn(move || {
            for _ in 0..N_ITERS {
                c.fetch_add(1, Ordering::Relaxed);
            }
        }));
    }
    for h in handles {
        h.join().unwrap();
    }
    let got = counter.load(Ordering::Relaxed);
    let expected = (N_TASKS * N_ITERS) as i32;
    println!("atomic path: counter={got} expected={expected}");
    assert_eq!(got, expected);
}
