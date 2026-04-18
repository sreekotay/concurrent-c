// Syscall Kidnapping Challenge - Zig version
// 100 threads call C sleep(2) blocking their OS thread.
// 1 heartbeat thread ticks every 100ms.
// Measures whether the OS can schedule the heartbeat despite blocked threads.

const std = @import("std");
const c = @cImport({
    @cInclude("unistd.h");
    @cInclude("time.h");
});

fn rawNanosleep(sec: c_long, nsec: c_long) void {
    var ts: c.struct_timespec = .{ .tv_sec = sec, .tv_nsec = nsec };
    _ = c.nanosleep(&ts, null);
}

const NUM_KIDNAPPERS = 100;
const TEST_DURATION_NS: u64 = 3 * std.time.ns_per_s;

var heartbeats: i64 = 0;
var kidnappers_done: i64 = 0;
var stop: i32 = 0;

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [4096]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    std.fs.File.stdout().writeAll(s) catch {};
}

fn kidnapper() void {
    // One kidnap unit: raw nanosleep(2s) blocks this kernel thread.
    rawNanosleep(2, 0);
    _ = @atomicRmw(i64, &kidnappers_done, .Add, 1, .monotonic);
}

fn heartbeatFn() void {
    while (@atomicLoad(i32, &stop, .seq_cst) == 0) {
        rawNanosleep(0, 100 * std.time.ns_per_ms);
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
    const final_done = @atomicLoad(i64, &kidnappers_done, .seq_cst);
    printf("\n=================================================================\n", .{});
    printf("FINAL RESULTS\n", .{});
    printf("Total Heartbeats:     {d}\n", .{final});
    printf("Kidnappers Completed: {d} / {d}\n", .{ final_done, NUM_KIDNAPPERS });
    printf("=================================================================\n", .{});
}
