//! Zig peer for stress/compare_arena_memory.sh — fair bump arena protocol.
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
    var buf: [512]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    _ = c.write(1, s.ptr, s.len);
}

fn alignUp(v: usize, a: usize) usize {
    return (v + a - 1) & ~(a - 1);
}

const Slab = struct {
    base: []u8,
    off: usize = 0,
    prev: ?*Slab = null,
};

const Bump = struct {
    cur: ?*Slab = null,
    nslabs: i32 = 0,
    block_max: i32 = 4,
    ovf: std.ArrayListUnmanaged([]u8) = .empty,
    ovf_bytes: usize = 0,
    live: usize = 0,
    gpa: std.mem.Allocator,

    fn init(self: *Bump, gpa: std.mem.Allocator, root: usize, block_max: i32) !void {
        self.* = .{ .gpa = gpa, .block_max = block_max };
        const s = try gpa.create(Slab);
        s.* = .{ .base = try gpa.alloc(u8, root) };
        self.cur = s;
        self.nslabs = 1;
    }

    fn reset(self: *Bump) void {
        for (self.ovf.items) |p| self.gpa.free(p);
        self.ovf.clearRetainingCapacity();
        self.ovf_bytes = 0;
        while (self.cur) |s| {
            if (s.prev == null) {
                s.off = 0;
                break;
            }
            const prev = s.prev;
            self.gpa.free(s.base);
            self.gpa.destroy(s);
            self.cur = prev;
            self.nslabs -= 1;
        }
        self.live = 0;
    }

    fn deinit(self: *Bump) void {
        self.reset();
        if (self.cur) |s| {
            self.gpa.free(s.base);
            self.gpa.destroy(s);
            self.cur = null;
        }
        self.ovf.deinit(self.gpa);
        self.nslabs = 0;
    }

    fn grow(self: *Bump, need: usize) !void {
        if (self.block_max > 0 and self.nslabs >= self.block_max) return error.Budget;
        const cur = self.cur orelse return error.NoSlab;
        var cap = cur.base.len + cur.base.len / 2;
        if (cap < need) cap = need;
        if (cap < 4096) cap = 4096;
        const ns = try self.gpa.create(Slab);
        ns.* = .{ .base = try self.gpa.alloc(u8, cap), .prev = cur };
        self.cur = ns;
        self.nslabs += 1;
    }

    fn alloc(self: *Bump, n: usize, aln: usize) ![]u8 {
        const cur0 = self.cur orelse return error.NoSlab;
        var off = alignUp(cur0.off, aln);
        var end = off + n;
        if (end > cur0.base.len) {
            self.grow(n + aln) catch {
                const p = try self.gpa.alloc(u8, n);
                try self.ovf.append(self.gpa, p);
                self.ovf_bytes += n;
                self.live += n;
                return p;
            };
            const c2 = self.cur.?;
            off = alignUp(c2.off, aln);
            end = off + n;
            const p = c2.base[off..end];
            c2.off = end;
            self.live += n;
            return p;
        }
        const p = cur0.base[off..end];
        cur0.off = end;
        self.live += n;
        return p;
    }

    fn realloc(self: *Bump, ptr: ?[]u8, old_n: usize, new_n: usize, aln: usize, moves: *u64) ![]u8 {
        if (ptr == null) return self.alloc(new_n, aln);
        const cur = self.cur orelse return error.NoSlab;
        const p = ptr.?;
        if (p.len > 0 and cur.base.len > 0) {
            const start = @intFromPtr(p.ptr) - @intFromPtr(cur.base.ptr);
            if (start < cur.base.len and start + old_n == cur.off) {
                const end = start + new_n;
                if (new_n <= old_n or end <= cur.base.len) {
                    cur.off = end;
                    self.live = self.live - old_n + new_n;
                    return cur.base[start..end];
                }
            }
        }
        const np = try self.alloc(new_n, aln);
        const copy_n = if (old_n < new_n) old_n else new_n;
        @memcpy(np[0..copy_n], p[0..copy_n]);
        moves.* += 1;
        return np;
    }

    fn gross(self: *const Bump) usize {
        var g = self.ovf_bytes;
        var s = self.cur;
        while (s) |sl| {
            g += sl.base.len;
            s = sl.prev;
        }
        return g;
    }
};

pub fn main() !void {
    const gpa = std.heap.c_allocator;

    const tip_iters = envZU("ARENA_MEM_TIP_ITERS", 20000);
    const tip_root = envZU("ARENA_MEM_TIP_ROOT", 4 * 1024 * 1024);
    const storm_n = envZU("ARENA_MEM_STORM", 50000);
    const storm_root = envZU("ARENA_MEM_ROOT_STORM", 4096);
    const churn_n = envZU("ARENA_MEM_CHURN", 200);
    const churn_each = envZU("ARENA_MEM_CHURN_EACH", 64);
    const block_max: i32 = @intCast(envZU("ARENA_MEM_BLOCK_MAX", 4));

    printf("arena_memory_bench(zig): block_max={d} tip_iters={d} storm={d}\n", .{ block_max, tip_iters, storm_n });

    var tip: Bump = undefined;
    try tip.init(gpa, tip_root, block_max);
    var moves: u64 = 0;
    var p: ?[]u8 = null;
    var sz: usize = 0;
    const t0 = nowMs();
    var i: usize = 0;
    while (i < tip_iters) : (i += 1) {
        const want = sz + 32 + (i % 17);
        p = try tip.realloc(p, sz, want, 8, &moves);
        sz = want;
    }
    const tip_ms = nowMs() - t0;
    tip.deinit();

    var storm: Bump = undefined;
    try storm.init(gpa, storm_root, block_max);
    const t1 = nowMs();
    i = 0;
    while (i < storm_n) : (i += 1) {
        const n = 24 + (i * 17) % 512;
        const q = try storm.alloc(n, 8);
        q[0] = @truncate(i);
    }
    const storm_ms = nowMs() - t1;
    const storm_gross = storm.gross();
    const storm_ovf = storm.ovf_bytes;
    storm.reset();
    const reset_gross = storm.gross();
    storm.deinit();

    const t2 = nowMs();
    i = 0;
    while (i < churn_n) : (i += 1) {
        var ch: Bump = undefined;
        try ch.init(gpa, 8 * 1024, block_max);
        var j: usize = 0;
        while (j < churn_each) : (j += 1) {
            const n = 16 + (j * 31) % 256;
            _ = try ch.alloc(n, 8);
        }
        ch.deinit();
    }
    const churn_ms = nowMs() - t2;

    printf(
        "RESULT lang=zig tip_ms={d:.3} tip_moves={d} storm_ms={d:.3} storm_gross={d} storm_ovf={d} reset_gross={d} churn_ms={d:.3} rss_delta=-1\n",
        .{ tip_ms, moves, storm_ms, storm_gross, storm_ovf, reset_gross, churn_ms },
    );
}
