// Noisy Neighbor Challenge - Zig version
// 15 CPU-hog threads + 1 heartbeat thread.
// Tests whether the OS scheduler can keep the heartbeat alive.

const std = @import("std");

const NUM_HOGS = 15;
const HEARTBEAT_INTERVAL_MS = 100;
const TEST_DURATION_NS: u64 = 5 * std.time.ns_per_s;

var heartbeats: i64 = 0;
var hogs_active: i64 = 0;
var stop: i32 = 0;

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [4096]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    std.fs.File.stdout().writeAll(s) catch {};
}

fn heartbeatFn() void {
    printf("[Heartbeat] Started\n", .{});
    while (@atomicLoad(i32, &stop, .seq_cst) == 0) {
        std.Thread.sleep(HEARTBEAT_INTERVAL_MS * std.time.ns_per_ms);
        const val = @atomicRmw(i64, &heartbeats, .Add, 1, .monotonic) + 1;
        printf("[Heartbeat] Tick {d}\n", .{val});
    }
}

fn hog(id: usize) void {
    _ = @atomicRmw(i64, &hogs_active, .Add, 1, .monotonic);
    printf("[Hog {d}] Started CPU-intensive loop...\n", .{id});

    var x: f64 = 1.1;
    while (@atomicLoad(i32, &stop, .seq_cst) == 0) {
        for (0..1_000_000) |_| {
            x = x * x;
            if (x > 1_000_000.0) x = 1.1;
        }
        std.mem.doNotOptimizeAway(x);
    }

    printf("[Hog {d}] Stopped\n", .{id});
    _ = @atomicRmw(i64, &hogs_active, .Sub, 1, .monotonic);
}

pub fn main() !void {
    const ncpu = std.Thread.getCpuCount() catch 4;
    printf("=================================================================\n", .{});
    printf("ZIG NOISY NEIGHBOR CHALLENGE\n", .{});
    printf("Workers: {d} | CPU Hogs: {d}\n", .{ ncpu, NUM_HOGS });
    printf("=================================================================\n\n", .{});

    const hb = try std.Thread.spawn(.{}, heartbeatFn, .{});

    std.Thread.sleep(500 * std.time.ns_per_ms);
    printf("Initial heartbeats: {d} (Healthy)\n", .{@atomicLoad(i64, &heartbeats, .seq_cst)});

    printf("\n!!! Unleashing CPU Hogs !!!\n", .{});

    var threads: [NUM_HOGS]std.Thread = undefined;
    for (0..NUM_HOGS) |i| {
        threads[i] = try std.Thread.spawn(.{}, hog, .{i});
    }

    std.Thread.sleep(TEST_DURATION_NS);
    @atomicStore(i32, &stop, 1, .seq_cst);

    for (0..NUM_HOGS) |i| {
        threads[i].join();
    }
    hb.join();

    const final = @atomicLoad(i64, &heartbeats, .seq_cst);
    printf("\n=================================================================\n", .{});
    printf("FINAL RESULTS\n", .{});
    printf("Total Heartbeats: {d}\n", .{final});

    const expected: i64 = 5 * 1000 / HEARTBEAT_INTERVAL_MS;
    if (final >= @divFloor(expected * 8, 10)) {
        printf("RESULT: PASS - Zig OS threads are fair even with CPU hogs!\n", .{});
    } else {
        printf("RESULT: FAIL - Zig was starved by CPU hogs.\n", .{});
        const eff = @as(f64, @floatFromInt(final)) * 100.0 / @as(f64, @floatFromInt(expected));
        printf("Heartbeat efficiency: {d:.1}%\n", .{eff});
    }
    printf("=================================================================\n", .{});
}
