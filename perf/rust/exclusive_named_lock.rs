//! exclusive_named_lock.rs — named exclusive sections (Rust)
//!
//! Same protocol as `perf/exclusive_named_lock.ccs`,
//! `perf/go/exclusive_named_lock.go`, and `perf/zig/exclusive_named_lock.zig`.
//!
//! Idiom: `std::sync::Mutex` per name, create-on-first-use via
//! `Mutex<HashMap<u64, Arc<Mutex<()>>>>`. OS threads via `thread::scope`.
//! Timing: before spawn through join (no start-gun).
//!
//! Build:
//!   rustc -O -C target-cpu=native exclusive_named_lock.rs -o exclusive_named_lock

use std::collections::HashMap;
use std::env;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Instant;

const DEFAULT_WORKERS: usize = 8;
const DEFAULT_ITERS: usize = 200_000;
const DEFAULT_TRIALS: usize = 7;
const DEFAULT_WORK: usize = 32;
const DEFAULT_KEYS: usize = 64;
const DEFAULT_ZIPF_S: f64 = 1.0;
const MAX_WORKERS: usize = 256;
const MAX_KEYS: usize = 4096;
const CACHE_LINE: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
struct Row {
    value: i64,
    _pad: [u8; CACHE_LINE - 8],
}

impl Default for Row {
    fn default() -> Self {
        Self {
            value: 0,
            _pad: [0; CACHE_LINE - 8],
        }
    }
}

struct Rng {
    state: u64,
}

impl Rng {
    fn for_worker(worker_id: usize) -> Self {
        let mut state = 0x9E3779B97F4A7C15u64 ^ (worker_id as u64).wrapping_mul(0xD1B54A32D192ED03);
        if state == 0 {
            state = 1;
        }
        Self { state }
    }

    fn next(&mut self) -> u64 {
        let mut x = self.state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.state = x;
        x
    }

    fn unit(&mut self) -> f64 {
        (self.next() >> 11) as f64 * (1.0 / 9007199254740992.0)
    }
}

fn env_usize(name: &str, fallback: usize, min: usize) -> usize {
    match env::var(name) {
        Ok(v) => v
            .parse::<usize>()
            .ok()
            .filter(|&n| n >= min)
            .unwrap_or(fallback),
        Err(_) => fallback,
    }
}

fn env_f64(name: &str, fallback: f64) -> f64 {
    match env::var(name) {
        Ok(v) => v
            .parse::<f64>()
            .ok()
            .filter(|&n| n > 0.0)
            .unwrap_or(fallback),
        Err(_) => fallback,
    }
}

fn mutex_for(dir: &Mutex<HashMap<u64, Arc<Mutex<()>>>>, name: u64) -> Arc<Mutex<()>> {
    let mut g = dir.lock().unwrap();
    g.entry(name)
        .or_insert_with(|| Arc::new(Mutex::new(())))
        .clone()
}

fn cs_work(spins: usize) {
    let mut x = 0i32;
    for i in 0..spins {
        x = x.wrapping_add(i as i32);
    }
    std::convert::identity(x);
}

fn row_bump(row: &mut Row, spins: usize) {
    let v = row.value;
    cs_work(spins);
    row.value = v + 1;
}

fn zipf_init(cum: &mut [f64], keys: usize, s: f64) {
    let mut total = 0.0;
    for k in 0..keys {
        let w = 1.0 / (k as f64 + 1.0).powf(s);
        total += w;
        cum[k] = total;
    }
    for k in 0..keys {
        cum[k] /= total;
    }
}

fn zipf_pick(cum: &[f64], keys: usize, rng: &mut Rng) -> usize {
    let u = rng.unit();
    let mut lo = 0usize;
    let mut hi = keys - 1;
    while lo < hi {
        let mid = (lo + hi) / 2;
        if cum[mid] < u {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    lo
}

fn median(samples: &[f64]) -> f64 {
    let mut s = samples.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    s[s.len() / 2]
}

fn run_zipf(
    dir: &Mutex<HashMap<u64, Arc<Mutex<()>>>>,
    rows: &mut [Row; MAX_KEYS],
    cum: &[f64],
    workers: usize,
    iters: usize,
    spins: usize,
    keys: usize,
) -> f64 {
    for row in rows.iter_mut().take(keys) {
        row.value = 0;
    }
    let rows_addr = rows.as_mut_ptr() as usize;

    let t0 = Instant::now();
    thread::scope(|scope| {
        for id in 0..workers {
            let cum = cum;
            scope.spawn(move || {
                let ms: Vec<Arc<Mutex<()>>> =
                    (0..keys).map(|k| mutex_for(dir, k as u64)).collect();
                let mut rng = Rng::for_worker(id);
                for _ in 0..iters {
                    let k = zipf_pick(cum, keys, &mut rng);
                    let _g = ms[k].lock().unwrap();
                    let rows = unsafe { &mut *(rows_addr as *mut [Row; MAX_KEYS]) };
                    row_bump(&mut rows[k], spins);
                }
            });
        }
    });
    let ms = t0.elapsed().as_secs_f64() * 1000.0;

    let sum: i64 = rows.iter().take(keys).map(|r| r.value).sum();
    let expect = (workers * iters) as i64;
    if sum != expect {
        eprintln!("ZIPF CHECK FAIL: got={sum} expect={expect}");
        std::process::exit(1);
    }
    ms
}

fn run_uncontended(
    dir: &Mutex<HashMap<u64, Arc<Mutex<()>>>>,
    rows: &mut [Row; MAX_KEYS],
    workers: usize,
    iters: usize,
    spins: usize,
) -> f64 {
    for row in rows.iter_mut().take(workers) {
        row.value = 0;
    }
    let rows_addr = rows.as_mut_ptr() as usize;

    let t0 = Instant::now();
    thread::scope(|scope| {
        for id in 0..workers {
            scope.spawn(move || {
                let m = mutex_for(dir, 1000 + id as u64);
                for _ in 0..iters {
                    let _g = m.lock().unwrap();
                    let rows = unsafe { &mut *(rows_addr as *mut [Row; MAX_KEYS]) };
                    row_bump(&mut rows[id], spins);
                }
            });
        }
    });
    let ms = t0.elapsed().as_secs_f64() * 1000.0;

    let sum: i64 = (0..workers).map(|w| rows[w].value).sum();
    let expect = (workers * iters) as i64;
    if sum != expect {
        eprintln!("UNCONTENDED CHECK FAIL: got={sum} expect={expect}");
        std::process::exit(1);
    }
    ms
}

fn main() {
    let mut workers = env_usize("CC_EXCL_WORKERS", DEFAULT_WORKERS, 1);
    let iters = env_usize("CC_EXCL_ITERS", DEFAULT_ITERS, 1);
    let trials = env_usize("CC_EXCL_TRIALS", DEFAULT_TRIALS, 1);
    let spins = env_usize("CC_EXCL_WORK", DEFAULT_WORK, 0);
    let mut keys = env_usize("CC_EXCL_KEYS", DEFAULT_KEYS, 1);
    let zipf_s = env_f64("CC_EXCL_ZIPF_S", DEFAULT_ZIPF_S);
    if workers > MAX_WORKERS {
        workers = MAX_WORKERS;
    }
    if keys > MAX_KEYS {
        keys = MAX_KEYS;
    }

    let dir: Mutex<HashMap<u64, Arc<Mutex<()>>>> = Mutex::new(HashMap::new());
    let mut rows: [Row; MAX_KEYS] = [Row::default(); MAX_KEYS];
    let mut cum = vec![0.0; keys];
    zipf_init(&mut cum, keys, zipf_s);

    println!("exclusive_named_lock lang=rust");
    println!(
        "  workers={workers} iters/worker={iters} timed_trials={trials} warmup=1 \
         cs_spins={spins} keys={keys} zipf_s={zipf_s:.3}"
    );
    println!("  total_lock_ops/trial={}", workers * iters);
    println!(
        "  note=std::sync::Mutex per name via HashMap; Zipf uses CS work; \
         uncontended is lock micro (spins=0); no start-gun; OS threads; \
         time includes spawn+join"
    );

    let warm = if iters > 1000 { 1000 } else { iters };
    let _ = run_zipf(&dir, &mut rows, &cum, workers, warm, spins, keys);
    let _ = run_uncontended(&dir, &mut rows, workers, warm, 0);

    let mut z_samples = vec![0.0; trials];
    let mut u_samples = vec![0.0; trials];
    for t in 0..trials {
        z_samples[t] = run_zipf(&dir, &mut rows, &cum, workers, iters, spins, keys);
        u_samples[t] = run_uncontended(&dir, &mut rows, workers, iters, 0);
        println!(
            "  trial {}: zipf_ms={:.3} uncontended_ms={:.3}",
            t + 1,
            z_samples[t],
            u_samples[t]
        );
    }

    let z_med = median(&z_samples);
    let u_med = median(&u_samples);
    let total_ops = (workers * iters) as f64;
    let z_ops = if z_med > 0.0 {
        total_ops / (z_med / 1000.0)
    } else {
        0.0
    };
    let u_ops = if u_med > 0.0 {
        total_ops / (u_med / 1000.0)
    } else {
        0.0
    };
    println!(
        "RESULT lang=rust zipf_median_ms={z_med:.3} zipf_ops_s={z_ops:.0} \
         uncontended_median_ms={u_med:.3} uncontended_ops_s={u_ops:.0}"
    );
}
