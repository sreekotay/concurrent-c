//! Track A driver — mirror of track_a/cc/driver.ccs checkpoint protocol.

mod hostile;

use hostile::*;
use std::env;
use std::time::Instant;

fn rss_bytes() -> usize {
    // Best-effort macOS/Linux RSS for PROTOCOL checkpoints (not compared
    // across implementations — oracle compares claim/weak/reg/ok fields).
    #[cfg(target_os = "macos")]
    {
        use std::mem::MaybeUninit;
        #[repr(C)]
        struct MachTaskBasicInfo {
            virtual_size: u64,
            resident_size: u64,
            resident_size_max: u64,
            user_time: [u32; 2],
            system_time: [u32; 2],
            policy: i32,
            suspend_count: i32,
        }
        const MACH_TASK_BASIC_INFO: u32 = 20;
        const MACH_TASK_BASIC_INFO_COUNT: u32 =
            (std::mem::size_of::<MachTaskBasicInfo>() / std::mem::size_of::<u32>()) as u32;
        extern "C" {
            fn mach_task_self() -> u32;
            fn task_info(
                target_task: u32,
                flavor: u32,
                task_info_out: *mut u32,
                task_info_count: *mut u32,
            ) -> i32;
        }
        unsafe {
            let mut info = MaybeUninit::<MachTaskBasicInfo>::uninit();
            let mut count = MACH_TASK_BASIC_INFO_COUNT;
            let kr = task_info(
                mach_task_self(),
                MACH_TASK_BASIC_INFO,
                info.as_mut_ptr() as *mut u32,
                &mut count,
            );
            if kr == 0 {
                return info.assume_init().resident_size as usize;
            }
        }
        0
    }
    #[cfg(target_os = "linux")]
    {
        if let Ok(s) = std::fs::read_to_string("/proc/self/statm") {
            let pages: usize = s.split_whitespace().nth(1).and_then(|x| x.parse().ok()).unwrap_or(0);
            return pages * 4096;
        }
        0
    }
    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    {
        0
    }
}

fn chk(epoch: i32, phase: &str, r: &HostileRealm, ok: i32, detail: Option<&str>) {
    let rss = rss_bytes();
    if let Some(d) = detail {
        if ok == 0 {
            println!(
                "CHK {{\"epoch\":{},\"phase\":\"{}\",\"claims\":{},\"weaks\":{},\"regs\":{},\"rss\":{},\"ok\":{},\"detail\":\"{}\"}}",
                epoch,
                phase,
                r.outstanding_claims(),
                r.outstanding_weaks(),
                r.outstanding_regs(),
                rss,
                ok,
                d
            );
            return;
        }
    }
    println!(
        "CHK {{\"epoch\":{},\"phase\":\"{}\",\"claims\":{},\"weaks\":{},\"regs\":{},\"rss\":{},\"ok\":{}}}",
        epoch,
        phase,
        r.outstanding_claims(),
        r.outstanding_weaks(),
        r.outstanding_regs(),
        rss,
        ok
    );
}

fn perf(op: &str, n: i32, ns: u128) {
    let per = if n > 0 { ns / n as u128 } else { 0 };
    println!(
        "PERF {{\"op\":\"{}\",\"n\":{},\"ns_total\":{},\"ns_per_op\":{}}}",
        op, n, ns, per
    );
}

const MAKE_PAYLOAD: &str = r#"
function makePayload(i) {
  const payload = new Uint8Array(64 * 1024);
  payload[0] = i & 255;
  const a = { i: i, payload: payload };
  const b = { parent: a };
  a.child = b;
  return function () {
    if (a.i !== i) throw new Error('corrupt');
    return a.payload[0];
  };
}
"#;

fn main() {
    let mut create = 200i32;
    let mut retain_n = 40i32;
    let mut gens = 3i32;
    let mut micro = 2000i32;
    match env::var("HOSTILE_SCALE").ok().as_deref() {
        Some("full") => {
            create = 1000;
            retain_n = 100;
            gens = 3;
            micro = 10000;
        }
        Some("x10") => {
            create = 10000;
            retain_n = 1000;
            gens = 3;
            micro = 100000;
        }
        Some("x100") => {
            create = 100000;
            retain_n = 10000;
            gens = 3;
            micro = 100000;
        }
        _ => {}
    }

    let realm = match HostileRealm::new() {
        Ok(r) => r,
        Err(e) => {
            eprintln!("realm_new failed {e}");
            std::process::exit(2);
        }
    };
    if realm.exec(MAKE_PAYLOAD).is_err() {
        eprintln!("inject makePayload failed");
        std::process::exit(2);
    }

    // microbench
    {
        let one = realm.eval("makePayload(0)").expect("eval");
        let t0 = Instant::now();
        for _ in 0..micro {
            let mut c = retain(&realm, &one).expect("retain");
            if release(&mut c) != HOSTILE_OK {
                std::process::exit(2);
            }
        }
        perf("retain", micro, t0.elapsed().as_nanos());

        let t0 = Instant::now();
        for _ in 0..micro {
            let mut c = retain(&realm, &one).expect("retain");
            let _ = release(&mut c);
        }
        perf("release", micro, t0.elapsed().as_nanos());

        {
            let mut c = retain(&realm, &one).expect("retain");
            let t0 = Instant::now();
            for _ in 0..micro {
                let mut b = borrow_begin(&c).expect("borrow");
                let _ = borrow_call_invoke(&b).expect("invoke");
                if borrow_end(&mut b) != HOSTILE_OK {
                    std::process::exit(2);
                }
            }
            perf("borrow", micro, t0.elapsed().as_nanos());
            let _ = release(&mut c);
        }
        one.drop_value();
    }

    let wall0 = Instant::now();
    for epoch in 0..gens {
        let mut claims: Vec<Option<HostileClaim>> = Vec::new();
        let mut weaks: Vec<Option<HostileWeak>> = Vec::new();
        let mut regs: Vec<Option<HostileRegistration>> = Vec::new();
        claims.resize_with(retain_n as usize, || None);
        weaks.resize_with(retain_n as usize, || None);

        for i in 0..create {
            let src = format!("makePayload({})", i + epoch * create);
            let v = match realm.eval(&src) {
                Ok(v) => v,
                Err(_) => {
                    chk(epoch, "after_create", &realm, 0, Some("eval"));
                    std::process::exit(3);
                }
            };
            if i < retain_n {
                let idx = i as usize;
                claims[idx] = Some(match retain(&realm, &v) {
                    Ok(c) => c,
                    Err(_) => {
                        chk(epoch, "after_retain", &realm, 0, Some("retain/weak"));
                        std::process::exit(3);
                    }
                });
                weaks[idx] = Some(match weak(&realm, &v) {
                    Ok(w) => w,
                    Err(_) => {
                        chk(epoch, "after_retain", &realm, 0, Some("retain/weak"));
                        std::process::exit(3);
                    }
                });
                if i % 4 == 0 {
                    regs.push(Some(match register_callback(&realm, &v) {
                        Ok(r) => r,
                        Err(_) => {
                            chk(epoch, "after_retain", &realm, 0, Some("register"));
                            std::process::exit(3);
                        }
                    }));
                }
            }
            v.drop_value();
        }
        chk(epoch, "after_retain", &realm, 1, None);

        let _ = realm.exec("globalThis.__batch = undefined");
        chk(epoch, "after_drop_roots", &realm, 1, None);

        realm.gc_pressure();
        chk(epoch, "after_gc", &realm, 1, None);

        for i in 0..retain_n as usize {
            let c = claims[i].as_ref().unwrap();
            let mut b = match borrow_begin(c) {
                Ok(b) => b,
                Err(_) => {
                    chk(epoch, "after_use", &realm, 0, Some("borrow"));
                    std::process::exit(3);
                }
            };
            if borrow_call_invoke(&b).is_err() {
                chk(epoch, "after_use", &realm, 0, Some("invoke_claim"));
                std::process::exit(3);
            }
            let _ = borrow_end(&mut b);
        }
        for reg in regs.iter() {
            if let Some(r) = reg {
                if invoke_registered(r) != HOSTILE_OK {
                    chk(epoch, "after_callback", &realm, 0, Some("invoke_reg"));
                    std::process::exit(3);
                }
            }
        }
        chk(epoch, "after_use", &realm, 1, None);
        chk(epoch, "after_callback", &realm, 1, None);

        for i in 0..(retain_n / 2) as usize {
            if let Some(ref mut c) = claims[i] {
                if release(c) != HOSTILE_OK {
                    chk(epoch, "after_release_half", &realm, 0, Some("release"));
                    std::process::exit(3);
                }
            }
            claims[i] = None;
        }
        chk(epoch, "after_release_half", &realm, 1, None);

        realm.gc_pressure();
        chk(epoch, "after_gc2", &realm, 1, None);

        for i in (retain_n / 2) as usize..retain_n as usize {
            let w = weaks[i].as_ref().unwrap();
            match upgrade(&realm, w) {
                Ok(mut up) => {
                    let _ = release(&mut up);
                }
                Err(_) => {
                    chk(epoch, "after_weak_check", &realm, 0, Some("upgrade_live"));
                    std::process::exit(3);
                }
            }
        }
        chk(epoch, "after_weak_check", &realm, 1, None);

        for reg in regs.iter_mut() {
            if let Some(ref mut r) = reg {
                if unregister(r) != HOSTILE_OK {
                    chk(epoch, "after_teardown", &realm, 0, Some("unregister"));
                    std::process::exit(3);
                }
                if invoke_registered(r) != HOSTILE_ERR_EXPIRED {
                    chk(epoch, "after_teardown", &realm, 0, Some("invoke_after_unreg"));
                    std::process::exit(3);
                }
            }
        }

        for i in (retain_n / 2) as usize..retain_n as usize {
            if let Some(ref mut c) = claims[i] {
                let _ = release(c);
            }
        }
        for w in weaks.iter_mut() {
            if let Some(ref mut ww) = w {
                weak_drop(ww);
            }
        }
    }

    realm.destroy();
    chk(gens, "after_teardown", &realm, 1, None);
    perf("workload_wall", 1, wall0.elapsed().as_nanos());
    println!("ORACLE PASS");
}
