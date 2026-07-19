//! Intentionally does not compile — this is the Rust *proof* win.
//!
//! Same intent as race_pointer_alias.ccs. In CC the share is still
//! explicit (`[p]` in the capture list); what fails open is safety of
//! the pointee, not visibility of the edge.
//!
//!   rustc examples/compare_rust_data_race/race_pointer_alias_rejected.rs
//!
//! Expect borrow / Send errors. CC accepts the pointer-alias version
//! and can lose updates at runtime.

use std::thread;

const N_TASKS: usize = 8;
const N_ITERS: usize = 100_000;

fn main() {
    let mut counter = 0i32;
    let mut handles = vec![];

    for _ in 0..N_TASKS {
        // Each task wants mutable access to the same cell.
        handles.push(thread::spawn(|| {
            for _ in 0..N_ITERS {
                counter += 1;
            }
        }));
    }

    for h in handles {
        h.join().unwrap();
    }
    println!("{counter}");
}
