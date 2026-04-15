// Thundering Herd Challenge - Zig version
// 1000 threads block on a condvar. Signal one, measure wake latency.

const std = @import("std");

const NUM_WAITERS = 1000;
const NUM_SAMPLES = 5;

var items: i64 = 0;
var count: i64 = 0;
var mu: std.Thread.Mutex = .{};
var cond: std.Thread.Condition = .{};

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [4096]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    std.fs.File.stdout().writeAll(s) catch {};
}

fn waiter() void {
    mu.lock();
    while (items <= 0) cond.wait(&mu);
    items -= 1;
    mu.unlock();
    _ = @atomicRmw(i64, &count, .Add, 1, .monotonic);
}

pub fn main() !void {
    printf("=================================================================\n", .{});
    printf("ZIG THUNDERING HERD CHALLENGE\n", .{});
    printf("Waiters: {d} | Samples: {d}\n", .{ NUM_WAITERS, NUM_SAMPLES });
    printf("=================================================================\n\n", .{});

    for (1..NUM_SAMPLES + 1) |sample| {
        @atomicStore(i64, &count, 0, .seq_cst);
        items = 0;

        var threads: [NUM_WAITERS]std.Thread = undefined;
        for (0..NUM_WAITERS) |i| {
            threads[i] = try std.Thread.spawn(.{}, waiter, .{});
        }

        std.Thread.sleep(100 * std.time.ns_per_ms);

        const start = std.time.nanoTimestamp();

        // Signal exactly one waiter
        {
            mu.lock();
            items += 1;
            cond.signal();
            mu.unlock();
        }

        // Spin-wait for the first waiter to finish
        while (@atomicLoad(i64, &count, .seq_cst) < 1) {
            std.atomic.spinLoopHint();
        }

        const elapsed = std.time.nanoTimestamp() - start;
        const latency_ms = @as(f64, @floatFromInt(elapsed)) / 1_000_000.0;

        printf("Sample {d}: Latency to wake 1st waiter: {d:8.4} ms\n", .{ sample, latency_ms });

        // Flush remaining waiters
        {
            mu.lock();
            items += NUM_WAITERS - 1;
            cond.broadcast();
            mu.unlock();
        }

        // Wait for all to complete
        for (0..NUM_WAITERS) |i| {
            threads[i].join();
        }
    }
}
