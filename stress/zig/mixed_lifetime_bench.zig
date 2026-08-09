//! Idiomatic mixed-lifetime peer: c_allocator malloc/free/realloc.
const std = @import("std");
const c = std.c;

fn envZU(comptime key: [:0]const u8, default: usize) usize {
    const v = c.getenv(key.ptr) orelse return default;
    return std.fmt.parseInt(usize, std.mem.span(v), 10) catch default;
}

fn nowMs() f64 {
    var ts: c.timespec = undefined;
    _ = c.clock_gettime(c.CLOCK.MONOTONIC, &ts);
    return @as(f64, @floatFromInt(ts.sec)) * 1e3 + @as(f64, @floatFromInt(ts.nsec)) * 1e-6;
}

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [768]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    _ = c.write(1, s.ptr, s.len);
}

fn itemSize(i: usize) usize {
    return 24 + (i * 17) % 400;
}

fn lcg(s: *u32) u32 {
    s.* = s.* *% 1664525 +% 1013904223;
    return s.*;
}

pub fn main() !void {
    const gpa = std.heap.c_allocator;
    const n = envZU("MIX_LIFE_N", 200000);
    var rng: u32 = @intCast(envZU("MIX_LIFE_SEED", 0xC0FFEE));

    const ptrs = try gpa.alloc(?[]u8, n);
    defer gpa.free(ptrs);
    const sizes = try gpa.alloc(usize, n);
    defer gpa.free(sizes);
    const alive = try gpa.alloc(bool, n);
    defer gpa.free(alive);
    const order = try gpa.alloc(usize, n);
    defer gpa.free(order);
    @memset(alive, false);

    printf("mixed_lifetime(zig malloc): n={d} seed={d}\n", .{ n, rng });

    var sink: usize = 0;
    const t0 = nowMs();

    var t = nowMs();
    var live: usize = 0;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        const sz = itemSize(i);
        const p = try gpa.alloc(u8, sz);
        p[0] = @truncate(i);
        sink ^= p[0];
        ptrs[i] = p;
        sizes[i] = sz;
        alive[i] = true;
        live += 1;
    }
    const alloc_ms = nowMs() - t;
    const peak_live = live;

    i = 0;
    while (i < n) : (i += 1) order[i] = i;
    i = n;
    while (i > 1) : (i -= 1) {
        const j = lcg(&rng) % @as(u32, @intCast(i));
        const tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }

    t = nowMs();
    var freed: usize = 0;
    i = 0;
    while (i < n / 2) : (i += 1) {
        const idx = order[i];
        if (!alive[idx]) continue;
        gpa.free(ptrs[idx].?);
        ptrs[idx] = null;
        alive[idx] = false;
        live -= 1;
        freed += 1;
    }
    const midfree_ms = nowMs() - t;

    t = nowMs();
    i = 0;
    while (i < n) : (i += 1) {
        if (!alive[i]) continue;
        const want = sizes[i] + 48 + (i % 64);
        const np = try gpa.realloc(ptrs[i].?, want);
        np[0] = @truncate(i ^ 0x5a);
        sink ^= np[0];
        ptrs[i] = np;
        sizes[i] = want;
    }
    const grow_ms = nowMs() - t;

    t = nowMs();
    i = 0;
    while (i < n) : (i += 1) {
        if (!alive[i]) continue;
        gpa.free(ptrs[i].?);
        ptrs[i] = null;
        alive[i] = false;
    }
    const reclaim_ms = nowMs() - t;
    const total_ms = nowMs() - t0;

    printf(
        "RESULT lang=zig strategy=malloc n={d} freed={d} peak_live={d} alloc_ms={d:.3} midfree_ms={d:.3} grow_ms={d:.3} reclaim_ms={d:.3} total_ms={d:.3} rss_peak_delta=-1 rss_end_delta=-1 hwm=0 cgroup_peak=0 sink={d}\n",
        .{ n, freed, peak_live, alloc_ms, midfree_ms, grow_ms, reclaim_ms, total_ms, sink },
    );
}
