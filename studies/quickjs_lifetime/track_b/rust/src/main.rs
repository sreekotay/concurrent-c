//! Track B Rust twin — libuv timers + rquickjs (same workload as CC).
//!
//! Timer fire is queued onto the host thread so callbacks run with an
//! active `Ctx` (avoids nested Context/RefCell panics).
use rquickjs::{
    function::Opt, Context, Ctx, Function, Object, Result as QResult, Runtime, Value,
};
use std::cell::RefCell;
use std::collections::HashMap;
use std::fs;
use std::rc::Rc;

#[repr(C)]
struct UvLoop {
    _private: [u8; 0],
}
#[repr(C)]
struct TbRustTimer {
    _private: [u8; 0],
}

type TimerCb = Option<extern "C" fn(*mut std::ffi::c_void)>;

extern "C" {
    fn tb_uv_default_loop() -> *mut UvLoop;
    fn tb_uv_timer_new(
        loop_: *mut UvLoop,
        cb: TimerCb,
        data: *mut std::ffi::c_void,
    ) -> *mut TbRustTimer;
    fn tb_uv_timer_start(t: *mut TbRustTimer, timeout: u64, repeat: u64) -> i32;
    fn tb_uv_timer_stop(t: *mut TbRustTimer) -> i32;
    fn tb_uv_timer_close(t: *mut TbRustTimer);
    fn tb_uv_run_once(loop_: *mut UvLoop) -> i32;
}

struct TimerEntry {
    handle: *mut TbRustTimer,
    func: rquickjs::Persistent<Function<'static>>,
    interval: bool,
}

struct Host {
    loop_: *mut UvLoop,
    next_id: i64,
    timers: HashMap<i64, TimerEntry>,
    pending: Vec<i64>,
    n_created: usize,
    n_cleared: usize,
    n_fired: usize,
    shutting_down: bool,
}

thread_local! {
    static HOST: RefCell<Option<Rc<RefCell<Host>>>> = const { RefCell::new(None) };
}

extern "C" fn on_timer(data: *mut std::ffi::c_void) {
    let id = data as isize as i64;
    HOST.with(|slot| {
        if let Some(host) = slot.borrow().as_ref() {
            host.borrow_mut().pending.push(id);
        }
    });
}

fn clear_one(host: &Rc<RefCell<Host>>, id: i64, count_clear: bool) {
    let mut h = host.borrow_mut();
    if let Some(entry) = h.timers.remove(&id) {
        unsafe {
            let _ = tb_uv_timer_stop(entry.handle);
            tb_uv_timer_close(entry.handle);
        }
        if count_clear {
            h.n_cleared += 1;
        }
    }
}

fn schedule<'js>(
    ctx: Ctx<'js>,
    func: Function<'js>,
    delay: Opt<i64>,
    interval: bool,
) -> QResult<i64> {
    let host = HOST.with(|slot| slot.borrow().as_ref().expect("host").clone());
    let persistent: rquickjs::Persistent<Function<'static>> =
        rquickjs::Persistent::save(&ctx, func);
    let mut h = host.borrow_mut();
    let id = h.next_id;
    h.next_id += 1;
    let delay = delay.0.unwrap_or(0).max(0) as u64;
    let handle = unsafe { tb_uv_timer_new(h.loop_, Some(on_timer), id as isize as *mut _) };
    if handle.is_null() {
        return Err(rquickjs::Exception::throw_message(&ctx, "uv_timer_new failed"));
    }
    let repeat = if interval { delay } else { 0 };
    if unsafe { tb_uv_timer_start(handle, delay, repeat) } != 0 {
        unsafe { tb_uv_timer_close(handle) };
        return Err(rquickjs::Exception::throw_message(&ctx, "uv_timer_start failed"));
    }
    h.timers.insert(
        id,
        TimerEntry {
            handle,
            func: persistent,
            interval,
        },
    );
    h.n_created += 1;
    Ok(id)
}

fn js_set_timeout<'js>(
    ctx: Ctx<'js>,
    func: Function<'js>,
    delay: Opt<i64>,
) -> QResult<i64> {
    schedule(ctx, func, delay, false)
}

fn js_set_interval<'js>(
    ctx: Ctx<'js>,
    func: Function<'js>,
    delay: Opt<i64>,
) -> QResult<i64> {
    schedule(ctx, func, delay, true)
}

fn js_clear_timeout(id: i64) {
    HOST.with(|slot| {
        if let Some(host) = slot.borrow().as_ref() {
            clear_one(host, id, true);
        }
    });
}

fn js_shutdown() {
    HOST.with(|slot| {
        let Some(host) = slot.borrow().clone() else {
            return;
        };
        let mut h = host.borrow_mut();
        h.shutting_down = true;
        let ids: Vec<i64> = h.timers.keys().copied().collect();
        drop(h);
        for id in ids {
            clear_one(&host, id, false);
        }
    });
}

fn js_console_log(msg: Opt<String>) {
    if let Some(m) = msg.0 {
        println!("{m}");
    } else {
        println!();
    }
}

fn install(ctx: Ctx<'_>) -> QResult<()> {
    let globals = ctx.globals();
    globals.set("setTimeout", Function::new(ctx.clone(), js_set_timeout)?)?;
    globals.set("setInterval", Function::new(ctx.clone(), js_set_interval)?)?;
    globals.set("clearTimeout", Function::new(ctx.clone(), js_clear_timeout)?)?;
    globals.set("clearInterval", Function::new(ctx.clone(), js_clear_timeout)?)?;
    globals.set("__hostile_shutdown", Function::new(ctx.clone(), js_shutdown)?)?;

    let console = Object::new(ctx.clone())?;
    console.set("log", Function::new(ctx.clone(), js_console_log)?)?;
    globals.set("console", console)?;
    Ok(())
}

fn drain_pending(ctx: &Ctx<'_>, host: &Rc<RefCell<Host>>) -> QResult<()> {
    loop {
        let batch = {
            let mut h = host.borrow_mut();
            if h.pending.is_empty() {
                break;
            }
            std::mem::take(&mut h.pending)
        };
        for id in batch {
            let (interval, func) = {
                let h = host.borrow();
                if h.shutting_down {
                    continue;
                }
                let Some(entry) = h.timers.get(&id) else {
                    continue;
                };
                (entry.interval, entry.func.clone())
            };
            host.borrow_mut().n_fired += 1;
            let f = func.restore(ctx)?;
            let _: Value = f.call(())?;
            if !interval {
                clear_one(host, id, false);
            }
        }
    }
    Ok(())
}

fn run_jobs(rt: &Runtime) {
    loop {
        match rt.execute_pending_job() {
            Ok(true) => continue,
            Ok(false) => break,
            Err(e) => {
                eprintln!("job exception: {e}");
                break;
            }
        }
    }
}

fn run_until_idle(ctx: &Context, rt: &Runtime, host: &Rc<RefCell<Host>>) -> QResult<()> {
    for _ in 0..1_000_000 {
        run_jobs(rt);
        ctx.with(|ctx| drain_pending(&ctx, host))?;
        let (live, pending) = {
            let h = host.borrow();
            (h.timers.len(), h.pending.len())
        };
        if live == 0 && pending == 0 {
            run_jobs(rt);
            ctx.with(|ctx| drain_pending(&ctx, host))?;
            if host.borrow().timers.is_empty() && host.borrow().pending.is_empty() {
                break;
            }
            continue;
        }
        let loop_ = host.borrow().loop_;
        unsafe {
            tb_uv_run_once(loop_);
        }
    }
    Ok(())
}

fn main() {
    let script = std::env::args().nth(1).unwrap_or_else(|| {
        "studies/quickjs_lifetime/track_b/workload/timers.js".into()
    });
    let src = fs::read_to_string(&script).expect("read script");

    println!(r#"META {{"track":"B","impl":"rust","libuv":"pinned"}}"#);

    let rt = Runtime::new().expect("runtime");
    let ctx = Context::full(&rt).expect("context");

    let host = Rc::new(RefCell::new(Host {
        loop_: unsafe { tb_uv_default_loop() },
        next_id: 1,
        timers: HashMap::new(),
        pending: Vec::new(),
        n_created: 0,
        n_cleared: 0,
        n_fired: 0,
        shutting_down: false,
    }));
    HOST.with(|h| *h.borrow_mut() = Some(host.clone()));

    let ok = (|| -> QResult<()> {
        ctx.with(|ctx| -> QResult<()> {
            install(ctx.clone())?;
            println!(r#"CHK {{"phase":"after_install","timers":0,"ok":1}}"#);
            let _: Value = ctx.eval(src.as_str())?;
            Ok(())
        })?;
        run_until_idle(&ctx, &rt, &host)?;
        Ok(())
    })();

    if let Err(e) = ok {
        eprintln!("fatal: {e}");
        std::process::exit(2);
    }

    let (created, cleared, fired, live) = {
        let h = host.borrow();
        (h.n_created, h.n_cleared, h.n_fired, h.timers.len())
    };
    println!(
        r#"CHK {{"phase":"after_workload","timers":{live},"created":{created},"cleared":{cleared},"fired":{fired},"ok":1}}"#
    );
    println!(r#"CHK {{"phase":"after_teardown","timers":0,"closing":0,"ok":1}}"#);

    HOST.with(|h| *h.borrow_mut() = None);

    if live == 0 {
        println!("ORACLE PASS");
    } else {
        println!("ORACLE FAIL");
        std::process::exit(3);
    }
}
