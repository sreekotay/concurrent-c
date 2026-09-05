//! Hostile retain/release/weak + callback registry — Rust/rquickjs.
//! Semantics: ../contract/hostile_contract.md

use rquickjs::{Context, Ctx, Function, Persistent, Runtime, Value};
use std::cell::RefCell;
use std::rc::Rc;

pub const HOSTILE_OK: i32 = 0;
pub const HOSTILE_ERR_EXPIRED: i32 = 1;
pub const HOSTILE_ERR_DOUBLE_RELEASE: i32 = 2;
pub const HOSTILE_ERR_STALE_REALM: i32 = 3;
pub const HOSTILE_ERR_STALE_BORROW: i32 = 4;
pub const HOSTILE_ERR_NOT_LIVE: i32 = 5;
pub const HOSTILE_ERR_BAD_VALUE: i32 = 6;

struct RealmInner {
    rt: Runtime,
    ctx: Context,
    alive: bool,
    n_claims: usize,
    n_weaks: usize,
    n_regs: usize,
}

pub struct HostileRealm {
    inner: Rc<RefCell<RealmInner>>,
}

pub struct HostileValue {
    realm: Rc<RefCell<RealmInner>>,
    val: Option<Persistent<Value<'static>>>,
}

pub struct HostileClaim {
    realm: Rc<RefCell<RealmInner>>,
    val: Option<Persistent<Value<'static>>>,
    live: bool,
}

pub struct HostileWeak {
    realm: Rc<RefCell<RealmInner>>,
    weakref: Option<Persistent<Value<'static>>>,
    live: bool,
}

pub struct HostileRegistration {
    realm: Rc<RefCell<RealmInner>>,
    fn_val: Option<Persistent<Value<'static>>>,
    live: bool,
}

pub struct HostileBorrow<'a> {
    claim: &'a HostileClaim,
    open: bool,
}

impl HostileRealm {
    pub fn new() -> Result<Self, i32> {
        let rt = Runtime::new().map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let ctx = Context::full(&rt).map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        Ok(Self {
            inner: Rc::new(RefCell::new(RealmInner {
                rt,
                ctx,
                alive: true,
                n_claims: 0,
                n_weaks: 0,
                n_regs: 0,
            })),
        })
    }

    pub fn destroy(&self) {
        let mut g = self.inner.borrow_mut();
        if !g.alive {
            return;
        }
        g.alive = false;
        // Dropping Context + Runtime happens when last Rc drops; clear
        // counters for post-teardown checkpoints.
        g.n_claims = 0;
        g.n_weaks = 0;
        g.n_regs = 0;
    }

    pub fn with<F, R>(&self, f: F) -> Result<R, i32>
    where
        F: for<'js> FnOnce(Ctx<'js>) -> Result<R, i32>,
    {
        let g = self.inner.borrow();
        if !g.alive {
            return Err(HOSTILE_ERR_STALE_REALM);
        }
        g.ctx.with(|ctx| f(ctx))
    }

    pub fn run_gc(&self) {
        let g = self.inner.borrow();
        if g.alive {
            g.rt.run_gc();
        }
    }

    pub fn outstanding_claims(&self) -> usize {
        self.inner.borrow().n_claims
    }
    pub fn outstanding_weaks(&self) -> usize {
        self.inner.borrow().n_weaks
    }
    pub fn outstanding_regs(&self) -> usize {
        self.inner.borrow().n_regs
    }

    pub fn eval(&self, src: &str) -> Result<HostileValue, i32> {
        let persistent = self.with(|ctx| -> Result<_, i32> {
            let v: Value = ctx.eval(src).map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
            Ok(Persistent::save(&ctx, v))
        })?;
        Ok(HostileValue {
            realm: Rc::clone(&self.inner),
            val: Some(persistent),
        })
    }

    pub fn exec(&self, src: &str) -> Result<(), i32> {
        self.with(|ctx| -> Result<(), i32> {
            let _: Value = ctx.eval(src).map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
            Ok(())
        })
    }

    pub fn gc_pressure(&self) {
        for _ in 0..8 {
            let _ = self.exec(
                "(function(){ let a=[]; for(let i=0;i<2000;i++) a.push(new Uint8Array(4096)); })()",
            );
            self.run_gc();
        }
    }
}

impl HostileValue {
    pub fn drop_value(mut self) {
        self.val = None;
    }
}

pub fn retain(realm: &HostileRealm, v: &HostileValue) -> Result<HostileClaim, i32> {
    if !Rc::ptr_eq(&realm.inner, &v.realm) {
        return Err(HOSTILE_ERR_BAD_VALUE);
    }
    let g = realm.inner.borrow();
    if !g.alive {
        return Err(HOSTILE_ERR_STALE_REALM);
    }
    let src = v.val.as_ref().ok_or(HOSTILE_ERR_BAD_VALUE)?;
    let dup: Persistent<Value<'static>> = g.ctx.with(|ctx| -> Result<_, i32> {
        let restored = src
            .clone()
            .restore(&ctx)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        Ok(Persistent::save(&ctx, restored))
    })?;
    drop(g);
    realm.inner.borrow_mut().n_claims += 1;
    Ok(HostileClaim {
        realm: Rc::clone(&realm.inner),
        val: Some(dup),
        live: true,
    })
}

pub fn release(claim: &mut HostileClaim) -> i32 {
    if !claim.live {
        return HOSTILE_ERR_DOUBLE_RELEASE;
    }
    {
        let g = claim.realm.borrow();
        if !g.alive {
            claim.live = false;
            claim.val = None;
            return HOSTILE_ERR_STALE_REALM;
        }
    }
    claim.live = false;
    claim.val = None;
    let mut g = claim.realm.borrow_mut();
    if g.n_claims > 0 {
        g.n_claims -= 1;
    }
    HOSTILE_OK
}

pub fn borrow_begin<'a>(claim: &'a HostileClaim) -> Result<HostileBorrow<'a>, i32> {
    if !claim.live {
        return Err(HOSTILE_ERR_NOT_LIVE);
    }
    if !claim.realm.borrow().alive {
        return Err(HOSTILE_ERR_STALE_REALM);
    }
    Ok(HostileBorrow {
        claim,
        open: true,
    })
}

pub fn borrow_call_invoke(b: &HostileBorrow<'_>) -> Result<i64, i32> {
    if !b.open || !b.claim.live {
        return Err(HOSTILE_ERR_STALE_BORROW);
    }
    let pers = b.claim.val.as_ref().ok_or(HOSTILE_ERR_NOT_LIVE)?;
    let g = b.claim.realm.borrow();
    if !g.alive {
        return Err(HOSTILE_ERR_STALE_REALM);
    }
    g.ctx.with(|ctx| -> Result<i64, i32> {
        let v = pers
            .clone()
            .restore(&ctx)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let f = Function::from_value(v).map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let out: i64 = f.call(()).map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        Ok(out)
    })
}

pub fn borrow_end(b: &mut HostileBorrow<'_>) -> i32 {
    if !b.open {
        return HOSTILE_ERR_STALE_BORROW;
    }
    b.open = false;
    HOSTILE_OK
}

pub fn weak(realm: &HostileRealm, v: &HostileValue) -> Result<HostileWeak, i32> {
    if !Rc::ptr_eq(&realm.inner, &v.realm) {
        return Err(HOSTILE_ERR_BAD_VALUE);
    }
    let g = realm.inner.borrow();
    if !g.alive {
        return Err(HOSTILE_ERR_STALE_REALM);
    }
    let src = v.val.as_ref().ok_or(HOSTILE_ERR_BAD_VALUE)?;
    let wr: Persistent<Value<'static>> = g.ctx.with(|ctx| -> Result<_, i32> {
        let restored = src
            .clone()
            .restore(&ctx)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        ctx.globals()
            .set("__hostile_tmp", restored)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let w: Value = ctx
            .eval("new WeakRef(__hostile_tmp)")
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let _: Value = ctx
            .eval("delete globalThis.__hostile_tmp")
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        Ok(Persistent::save(&ctx, w))
    })?;
    drop(g);
    realm.inner.borrow_mut().n_weaks += 1;
    Ok(HostileWeak {
        realm: Rc::clone(&realm.inner),
        weakref: Some(wr),
        live: true,
    })
}

pub fn weak_drop(w: &mut HostileWeak) {
    if !w.live {
        return;
    }
    w.live = false;
    w.weakref = None;
    let mut g = w.realm.borrow_mut();
    if g.n_weaks > 0 {
        g.n_weaks -= 1;
    }
}

pub fn upgrade(realm: &HostileRealm, w: &HostileWeak) -> Result<HostileClaim, i32> {
    if !w.live {
        return Err(HOSTILE_ERR_EXPIRED);
    }
    if !Rc::ptr_eq(&realm.inner, &w.realm) {
        return Err(HOSTILE_ERR_BAD_VALUE);
    }
    let g = realm.inner.borrow();
    if !g.alive {
        return Err(HOSTILE_ERR_STALE_REALM);
    }
    let wr = w.weakref.as_ref().ok_or(HOSTILE_ERR_EXPIRED)?;
    let maybe: Persistent<Value<'static>> = g.ctx.with(|ctx| -> Result<_, i32> {
        let restored = wr
            .clone()
            .restore(&ctx)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        ctx.globals()
            .set("__hostile_wr", restored)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let d: Value = ctx
            .eval("(__hostile_wr.deref())")
            .map_err(|_| HOSTILE_ERR_EXPIRED)?;
        let _: Value = ctx
            .eval("delete globalThis.__hostile_wr")
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        if d.is_undefined() || d.is_null() {
            return Err(HOSTILE_ERR_EXPIRED);
        }
        Ok(Persistent::save(&ctx, d))
    })?;
    drop(g);
    let hv = HostileValue {
        realm: Rc::clone(&realm.inner),
        val: Some(maybe),
    };
    retain(realm, &hv)
}

pub fn register_callback(
    realm: &HostileRealm,
    fn_v: &HostileValue,
) -> Result<HostileRegistration, i32> {
    if !Rc::ptr_eq(&realm.inner, &fn_v.realm) {
        return Err(HOSTILE_ERR_BAD_VALUE);
    }
    let g = realm.inner.borrow();
    if !g.alive {
        return Err(HOSTILE_ERR_STALE_REALM);
    }
    let src = fn_v.val.as_ref().ok_or(HOSTILE_ERR_BAD_VALUE)?;
    let dup: Persistent<Value<'static>> = g.ctx.with(|ctx| -> Result<_, i32> {
        let restored = src
            .clone()
            .restore(&ctx)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        if !restored.is_function() {
            return Err(HOSTILE_ERR_BAD_VALUE);
        }
        Ok(Persistent::save(&ctx, restored))
    })?;
    drop(g);
    realm.inner.borrow_mut().n_regs += 1;
    Ok(HostileRegistration {
        realm: Rc::clone(&realm.inner),
        fn_val: Some(dup),
        live: true,
    })
}

pub fn unregister(reg: &mut HostileRegistration) -> i32 {
    if !reg.live {
        return HOSTILE_ERR_DOUBLE_RELEASE;
    }
    {
        let g = reg.realm.borrow();
        if !g.alive {
            reg.live = false;
            reg.fn_val = None;
            return HOSTILE_ERR_STALE_REALM;
        }
    }
    reg.live = false;
    reg.fn_val = None;
    let mut g = reg.realm.borrow_mut();
    if g.n_regs > 0 {
        g.n_regs -= 1;
    }
    HOSTILE_OK
}

pub fn invoke_registered(reg: &HostileRegistration) -> i32 {
    if !reg.live {
        return HOSTILE_ERR_EXPIRED;
    }
    let g = reg.realm.borrow();
    if !g.alive {
        return HOSTILE_ERR_STALE_REALM;
    }
    let pers = match reg.fn_val.as_ref() {
        Some(p) => p,
        None => return HOSTILE_ERR_EXPIRED,
    };
    match g.ctx.with(|ctx| -> Result<(), i32> {
        let v = pers
            .clone()
            .restore(&ctx)
            .map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let f = Function::from_value(v).map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        let _: Value = f.call(()).map_err(|_| HOSTILE_ERR_BAD_VALUE)?;
        Ok(())
    }) {
        Ok(()) => HOSTILE_OK,
        Err(e) => e,
    }
}
