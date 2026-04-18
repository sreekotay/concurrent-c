// Thundering Herd Challenge - Zig version
// 1000 threads park on a condvar. Signal one, measure wake latency.
//
// Uses pthread_cond_t / pthread_mutex_t via C interop rather than
// std.Thread.Condition (which on macOS is a pure-futex impl whose
// wake semantics differ enough from pthread_cond_signal that it
// stops being an apples-to-apples comparison with the C baseline).
//
// Idiom matches pthread_herd_baseline.c one-to-one:
//   - waiter: lock(mu); while (items == 0) cond_wait(cv, mu); items--;
//             unlock(mu); atomic inc(count);
//   - signal: lock(mu); items++; cond_signal(cv); unlock(mu);

const std = @import("std");
const c = @cImport({
    @cInclude("pthread.h");
    @cInclude("time.h");
    @cInclude("unistd.h");
});

const NUM_WAITERS = 1000;
const NUM_SAMPLES = 5;

var mu: c.pthread_mutex_t = undefined;
var cv: c.pthread_cond_t = undefined;
var items: i64 = 0;
var count: i64 = 0;

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [4096]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    std.fs.File.stdout().writeAll(s) catch {};
}

fn waiter(_: usize) callconv(.c) ?*anyopaque {
    _ = c.pthread_mutex_lock(&mu);
    while (items == 0) {
        _ = c.pthread_cond_wait(&cv, &mu);
    }
    items -= 1;
    _ = c.pthread_mutex_unlock(&mu);
    _ = @atomicRmw(i64, &count, .Add, 1, .monotonic);
    return null;
}

// Trampoline matching pthread_create(void*(*)(void*), void*).
fn waiter_thread(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    return waiter(@intFromPtr(arg));
}

fn time_now_ms() f64 {
    var ts: c.timespec = undefined;
    _ = c.clock_gettime(c.CLOCK_MONOTONIC, &ts);
    return @as(f64, @floatFromInt(ts.tv_sec)) * 1000.0 +
        @as(f64, @floatFromInt(ts.tv_nsec)) / 1_000_000.0;
}

pub fn main() !void {
    printf("=================================================================\n", .{});
    printf("ZIG THUNDERING HERD CHALLENGE (pthread_cond, via C interop)\n", .{});
    printf("Waiters: {d} | Samples: {d}\n", .{ NUM_WAITERS, NUM_SAMPLES });
    printf("=================================================================\n\n", .{});

    for (1..NUM_SAMPLES + 1) |sample| {
        _ = c.pthread_mutex_init(&mu, null);
        _ = c.pthread_cond_init(&cv, null);
        items = 0;
        @atomicStore(i64, &count, 0, .seq_cst);

        var threads: [NUM_WAITERS]c.pthread_t = undefined;
        for (0..NUM_WAITERS) |i| {
            _ = c.pthread_create(&threads[i], null, waiter_thread, null);
        }

        _ = c.usleep(100_000); // 100ms — let everyone park

        const start = time_now_ms();
        _ = c.pthread_mutex_lock(&mu);
        items += 1;
        _ = c.pthread_cond_signal(&cv); // wakes exactly one waiter
        _ = c.pthread_mutex_unlock(&mu);

        while (@atomicLoad(i64, &count, .seq_cst) < 1) {
            std.atomic.spinLoopHint();
        }
        const latency_ms = time_now_ms() - start;
        printf("Sample {d}: Latency to wake 1st waiter: {d:8.4} ms\n", .{ sample, latency_ms });

        // Flush the rest so the waiters can exit and we can join them.
        _ = c.pthread_mutex_lock(&mu);
        items += NUM_WAITERS - 1;
        _ = c.pthread_cond_broadcast(&cv);
        _ = c.pthread_mutex_unlock(&mu);
        for (0..NUM_WAITERS) |i| {
            _ = c.pthread_join(threads[i], null);
        }

        _ = c.pthread_cond_destroy(&cv);
        _ = c.pthread_mutex_destroy(&mu);
    }
}
