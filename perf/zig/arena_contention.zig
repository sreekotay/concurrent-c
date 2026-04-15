// Arena Contention Challenge - Zig version
// 16 threads each perform 62500 heap allocations.
// Measures allocation throughput using c_allocator (malloc/free).

const std = @import("std");

const NUM_THREADS = 16;
const ALLOCS_PER_THREAD = 62500;

var success_count: i64 = 0;

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [4096]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    std.fs.File.stdout().writeAll(s) catch {};
}

fn mallocWorker() void {
    const alloc = std.heap.c_allocator;
    var local_success: i64 = 0;
    for (0..ALLOCS_PER_THREAD) |i| {
        const buf = alloc.alloc(u8, 16) catch continue;
        buf[0] = @truncate(i);
        std.mem.doNotOptimizeAway(buf.ptr);
        alloc.free(buf);
        local_success += 1;
    }
    _ = @atomicRmw(i64, &success_count, .Add, local_success, .monotonic);
}

pub fn main() !void {
    printf("=================================================================\n", .{});
    printf("ZIG HEAP ALLOCATION CHALLENGE\n", .{});
    printf("Threads: {d} | Allocs per thread: {d}\n", .{ NUM_THREADS, ALLOCS_PER_THREAD });
    printf("=================================================================\n\n", .{});

    const start = std.time.nanoTimestamp();

    var threads: [NUM_THREADS]std.Thread = undefined;
    for (0..NUM_THREADS) |i| {
        threads[i] = try std.Thread.spawn(.{}, mallocWorker, .{});
    }
    for (0..NUM_THREADS) |i| {
        threads[i].join();
    }

    const elapsed = std.time.nanoTimestamp() - start;
    const success = @atomicLoad(i64, &success_count, .seq_cst);
    const elapsed_ms = @as(f64, @floatFromInt(elapsed)) / 1_000_000.0;
    const elapsed_sec = elapsed_ms / 1000.0;
    const throughput = @as(f64, @floatFromInt(success)) / elapsed_sec / 1_000_000.0;

    printf("Results:\n", .{});
    printf("  Success: {d}\n", .{success});
    printf("  Time:    {d:.2} ms\n", .{elapsed_ms});
    printf("  Throughput: {d:.2} M allocs/sec\n", .{throughput});
    printf("=================================================================\n", .{});
}
