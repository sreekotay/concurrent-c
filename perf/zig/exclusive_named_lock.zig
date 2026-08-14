// exclusive_named_lock.zig — named exclusive sections (Zig)
//
// Same protocol as perf/exclusive_named_lock.ccs,
// perf/go/exclusive_named_lock.go, and perf/rust/exclusive_named_lock.rs.
//
// Idiom: std.Thread.Mutex per name, create-on-first-use via AutoHashMap
// under std.Thread.RwLock (shared read on the hot path). Zipf locks by
// name each iteration (directory lookup on the hot path). OS threads
// (std.Thread.spawn + join). Timing is before spawn through join (no
// start-gun). std.Thread rather than std.Io so this builds on Zig 0.15
// and 0.16 (Io.Threaded is 0.16-only).
//
// Build:
//   zig build-exe exclusive_named_lock.zig -O ReleaseFast -lc \
//     -femit-bin=exclusive_named_lock

const std = @import("std");
const c = std.c;

const DEFAULT_WORKERS: usize = 8;
const DEFAULT_ITERS: usize = 200_000;
const DEFAULT_TRIALS: usize = 7;
const DEFAULT_WORK: usize = 32;
const DEFAULT_KEYS: usize = 64;
const DEFAULT_ZIPF_S: f64 = 1.0;
const MAX_WORKERS: usize = 256;
const MAX_KEYS: usize = 4096;
const CACHE_LINE: usize = 64;

const Row = extern struct {
    value: i64 = 0,
    _pad: [CACHE_LINE - 8]u8 = [_]u8{0} ** (CACHE_LINE - 8),
};

const Rng = struct {
    state: u64,

    fn forWorker(worker_id: usize) Rng {
        var state: u64 = 0x9E3779B97F4A7C15 ^ (@as(u64, @intCast(worker_id)) *% 0xD1B54A32D192ED03);
        if (state == 0) state = 1;
        return .{ .state = state };
    }

    fn next(self: *Rng) u64 {
        var x = self.state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.state = x;
        return x;
    }

    fn unit(self: *Rng) f64 {
        return @as(f64, @floatFromInt(self.next() >> 11)) * (1.0 / 9007199254740992.0);
    }
};

var dir_rw: std.Thread.RwLock = .{};
var dir: ?std.AutoHashMap(u64, *std.Thread.Mutex) = null;
var rows: [MAX_KEYS]Row = [_]Row{.{}} ** MAX_KEYS;
var zipf_cum: [MAX_KEYS]f64 = [_]f64{0} ** MAX_KEYS;

fn envUsize(name: [*:0]const u8, fallback: usize, min: usize) usize {
    const val = c.getenv(name) orelse return fallback;
    const n = std.fmt.parseInt(usize, std.mem.span(val), 10) catch return fallback;
    return if (n < min) fallback else n;
}

fn envF64(name: [*:0]const u8, fallback: f64) f64 {
    const val = c.getenv(name) orelse return fallback;
    const n = std.fmt.parseFloat(f64, std.mem.span(val)) catch return fallback;
    return if (n <= 0) fallback else n;
}

fn ensureDir() *std.AutoHashMap(u64, *std.Thread.Mutex) {
    if (dir) |*d| return d;
    dir = std.AutoHashMap(u64, *std.Thread.Mutex).init(std.heap.page_allocator);
    return &dir.?;
}

fn mutexFor(name: u64) !*std.Thread.Mutex {
    dir_rw.lockShared();
    if (dir) |*d| {
        if (d.get(name)) |m| {
            dir_rw.unlockShared();
            return m;
        }
    }
    dir_rw.unlockShared();

    dir_rw.lock();
    defer dir_rw.unlock();
    const d = ensureDir();
    if (d.get(name)) |m| return m;
    const m = try std.heap.page_allocator.create(std.Thread.Mutex);
    m.* = .{};
    try d.put(name, m);
    return m;
}

fn csWork(spins: usize) void {
    // doNotOptimizeAway each iteration: with it only at the end, LLVM
    // replaces the loop with a closed-form sum and the CS does no work.
    var x: i32 = 0;
    var i: usize = 0;
    while (i < spins) : (i += 1) {
        x +%= @intCast(i);
        std.mem.doNotOptimizeAway(x);
    }
}

fn rowBump(r: *Row, spins: usize) void {
    const v = r.value;
    csWork(spins);
    r.value = v + 1;
}

fn zipfInit(cum: []f64, keys: usize, s: f64) void {
    var total: f64 = 0;
    var k: usize = 0;
    while (k < keys) : (k += 1) {
        const w = 1.0 / std.math.pow(f64, @as(f64, @floatFromInt(k + 1)), s);
        total += w;
        cum[k] = total;
    }
    k = 0;
    while (k < keys) : (k += 1) cum[k] /= total;
}

fn zipfPick(cum: []const f64, keys: usize, r: *Rng) usize {
    const u = r.unit();
    var lo: usize = 0;
    var hi: usize = keys - 1;
    while (lo < hi) {
        const mid = (lo + hi) / 2;
        if (cum[mid] < u) lo = mid + 1 else hi = mid;
    }
    return lo;
}

fn median(samples: []f64) f64 {
    var tmp: [64]f64 = undefined;
    if (samples.len > tmp.len) @panic("too many trials");
    @memcpy(tmp[0..samples.len], samples);
    const s = tmp[0..samples.len];
    std.mem.sort(f64, s, {}, std.sort.asc(f64));
    return s[s.len / 2];
}

fn monoNs() i64 {
    var ts: c.timespec = undefined;
    _ = c.clock_gettime(c.CLOCK.MONOTONIC, &ts);
    return @as(i64, ts.sec) * 1_000_000_000 + ts.nsec;
}

const ZipfCtx = struct {
    worker_id: usize,
    iters: usize,
    spins: usize,
    keys: usize,
};

fn zipfWorker(ctx: ZipfCtx) void {
    var r = Rng.forWorker(ctx.worker_id);
    var i: usize = 0;
    while (i < ctx.iters) : (i += 1) {
        const name = zipfPick(zipf_cum[0..ctx.keys], ctx.keys, &r);
        const m = mutexFor(name) catch @panic("mutexFor");
        m.lock();
        rowBump(&rows[name], ctx.spins);
        m.unlock();
    }
}

fn runZipf(workers: usize, iters: usize, spins: usize, keys: usize) !f64 {
    var k: usize = 0;
    while (k < keys) : (k += 1) rows[k].value = 0;

    var threads: [MAX_WORKERS]std.Thread = undefined;
    const t0 = monoNs();
    for (0..workers) |w| {
        threads[w] = try std.Thread.spawn(.{}, zipfWorker, .{ZipfCtx{
            .worker_id = w,
            .iters = iters,
            .spins = spins,
            .keys = keys,
        }});
    }
    for (0..workers) |w| threads[w].join();
    const ms = @as(f64, @floatFromInt(monoNs() - t0)) / 1_000_000.0;

    var sum: i64 = 0;
    k = 0;
    while (k < keys) : (k += 1) sum += rows[k].value;
    const expect: i64 = @intCast(workers * iters);
    if (sum != expect) {
        _ = c.printf("ZIPF CHECK FAIL: got=%lld expect=%lld\n", sum, expect);
        std.process.exit(1);
    }
    return ms;
}

fn runSerialFastpath(iters: usize) !f64 {
    rows[0].value = 0;
    const m = try mutexFor(1000);
    const t0 = monoNs();
    var i: usize = 0;
    while (i < iters) : (i += 1) {
        m.lock();
        rowBump(&rows[0], 0);
        m.unlock();
    }
    const ms = @as(f64, @floatFromInt(monoNs() - t0)) / 1_000_000.0;

    const expect: i64 = @intCast(iters);
    if (rows[0].value != expect) {
        _ = c.printf("FASTPATH CHECK FAIL: got=%lld expect=%lld\n", rows[0].value, expect);
        std.process.exit(1);
    }
    return ms;
}

pub fn main() !void {
    var workers = envUsize("CC_EXCL_WORKERS", DEFAULT_WORKERS, 1);
    const iters = envUsize("CC_EXCL_ITERS", DEFAULT_ITERS, 1);
    const trials = envUsize("CC_EXCL_TRIALS", DEFAULT_TRIALS, 1);
    const spins = envUsize("CC_EXCL_WORK", DEFAULT_WORK, 0);
    var keys = envUsize("CC_EXCL_KEYS", DEFAULT_KEYS, 1);
    const zipf_s = envF64("CC_EXCL_ZIPF_S", DEFAULT_ZIPF_S);
    if (workers > MAX_WORKERS) workers = MAX_WORKERS;
    if (keys > MAX_KEYS) keys = MAX_KEYS;
    if (trials > 64) {
        _ = c.printf("CC_EXCL_TRIALS too large (max 64)\n");
        std.process.exit(1);
    }
    zipfInit(zipf_cum[0..keys], keys, zipf_s);

    _ = c.printf("exclusive_named_lock lang=zig\n");
    _ = c.printf(
        "  workers=%zu iters/worker=%zu timed_trials=%zu warmup=1 cs_spins=%zu keys=%zu zipf_s=%.3f\n",
        workers,
        iters,
        trials,
        spins,
        keys,
        zipf_s,
    );
    _ = c.printf("  total_lock_ops/trial=%zu\n", workers * iters);
    _ = c.printf("  note=zipf locks BY NAME each iter (directory+lock+scheduler product, spawn+join, no start-gun, OS threads); serial_fastpath is one resolved mutex, one caller, spins=0, no scheduler\n");

    const warm = if (iters > 1000) @as(usize, 1000) else iters;
    _ = try runZipf(workers, warm, spins, keys);
    _ = try runSerialFastpath(workers * warm);

    var z_samples: [64]f64 = undefined;
    var f_samples: [64]f64 = undefined;
    const total_ops = workers * iters;
    for (0..trials) |t| {
        z_samples[t] = try runZipf(workers, iters, spins, keys);
        f_samples[t] = try runSerialFastpath(total_ops);
        _ = c.printf("  trial %zu: zipf_ms=%.3f serial_fastpath_ms=%.3f\n", t + 1, z_samples[t], f_samples[t]);
    }

    const z_med = median(z_samples[0..trials]);
    const f_med = median(f_samples[0..trials]);
    const ops: f64 = @floatFromInt(total_ops);
    const z_ops = if (z_med > 0) ops / (z_med / 1000.0) else 0.0;
    const f_ops = if (f_med > 0) ops / (f_med / 1000.0) else 0.0;
    _ = c.printf(
        "RESULT lang=zig zipf_median_ms=%.3f zipf_ops_s=%.0f serial_fastpath_median_ms=%.3f serial_fastpath_ops_s=%.0f\n",
        z_med,
        z_ops,
        f_med,
        f_ops,
    );
}
