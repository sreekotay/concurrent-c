// Syscall Kidnapping Challenge - Zig version
// 100 threads call C sleep(2) blocking their OS thread.
// 1 heartbeat thread ticks every 100ms.
// Measures whether the OS can schedule the heartbeat despite blocked threads.

const std = @import("std");
const c = @cImport({
    @cInclude("unistd.h");
});

const NUM_KIDNAPPERS = 100;
const TEST_DURATION_NS: u64 = 5 * std.time.ns_per_s;

var heartbeats: i64 = 0;
var stop: i32 = 0;

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [4096]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    std.fs.File.stdout().writeAll(s) catch {};
}

fn kidnapper() void {
    while (@atomicLoad(i32, &stop, .seq_cst) == 0) {
        _ = c.sleep(2);
    }
}

fn heartbeatFn() void {
    while (@atomicLoad(i32, &stop, .seq_cst) == 0) {
        std.Thread.sleep(100 * std.time.ns_per_ms);
        const val = @atomicRmw(i64, &heartbeats, .Add, 1, .monotonic) + 1;
        printf("[Heartbeat] Tick {d}\n", .{val});
    }
}

pub fn main() !void {
    printf("=================================================================\n", .{});
    printf("ZIG SYSCALL KIDNAPPING CHALLENGE\n", .{});
    printf("Kidnappers: {d} | Duration: 5s\n", .{NUM_KIDNAPPERS});
    printf("=================================================================\n\n", .{});

    const hb = try std.Thread.spawn(.{}, heartbeatFn, .{});

    std.Thread.sleep(500 * std.time.ns_per_ms);

    printf("\n!!! Unleashing Kidnappers (C.sleep blocking OS threads) !!!\n", .{});

    var threads: [NUM_KIDNAPPERS]std.Thread = undefined;
    for (0..NUM_KIDNAPPERS) |i| {
        threads[i] = try std.Thread.spawn(.{}, kidnapper, .{});
    }

    std.Thread.sleep(TEST_DURATION_NS);
    @atomicStore(i32, &stop, 1, .seq_cst);

    hb.join();
    for (0..NUM_KIDNAPPERS) |i| {
        threads[i].join();
    }

    const final = @atomicLoad(i64, &heartbeats, .seq_cst);
    printf("\n=================================================================\n", .{});
    printf("FINAL RESULTS\n", .{});
    printf("Total Heartbeats: {d}\n", .{final});
    printf("=================================================================\n", .{});
}
