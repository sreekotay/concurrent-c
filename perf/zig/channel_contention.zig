// Channel Isolation / Contention Challenge - Zig version
// Bounded channel with configurable producers/consumers.
// Measures baseline (1x1) vs contention (NxM) throughput.

const std = @import("std");
const Io = std.Io;

const DEFAULT_MESSAGES: usize = 1_000_000;
const DEFAULT_TRIALS: usize = 15;
const DEFAULT_PRODUCERS: usize = 8;
const DEFAULT_CONSUMERS: usize = 8;
const CHAN_CAP: usize = 1024;

var sink: i64 = 0;
var g_io: Io = undefined;

fn printf(comptime fmt: []const u8, args: anytype) void {
    var buf: [4096]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    _ = std.c.write(1, s.ptr, s.len);
}

fn envIntOrDefault(name: [*:0]const u8, default: usize) usize {
    const val = std.c.getenv(name) orelse return default;
    return std.fmt.parseInt(usize, std.mem.span(val), 10) catch default;
}

fn monoNs() i64 {
    var ts: std.c.timespec = undefined;
    _ = std.c.clock_gettime(std.c.CLOCK.MONOTONIC, &ts);
    return @as(i64, ts.sec) * 1_000_000_000 + ts.nsec;
}

fn BoundedQueue(comptime T: type, comptime cap: usize) type {
    return struct {
        buffer: [cap]T = undefined,
        head: usize = 0,
        tail: usize = 0,
        len: usize = 0,
        closed: bool = false,
        mu: Io.Mutex = .init,
        not_full: Io.Condition = .init,
        not_empty: Io.Condition = .init,

        const Self = @This();

        fn push(self: *Self, item: T) void {
            self.mu.lockUncancelable(g_io);
            defer self.mu.unlock(g_io);
            while (self.len == cap) self.not_full.waitUncancelable(g_io, &self.mu);
            self.buffer[self.tail] = item;
            self.tail = (self.tail + 1) % cap;
            self.len += 1;
            self.not_empty.signal(g_io);
        }

        fn pop(self: *Self) ?T {
            self.mu.lockUncancelable(g_io);
            defer self.mu.unlock(g_io);
            while (self.len == 0) {
                if (self.closed) return null;
                self.not_empty.waitUncancelable(g_io, &self.mu);
            }
            const item = self.buffer[self.head];
            self.head = (self.head + 1) % cap;
            self.len -= 1;
            self.not_full.signal(g_io);
            return item;
        }

        fn close(self: *Self) void {
            self.mu.lockUncancelable(g_io);
            defer self.mu.unlock(g_io);
            self.closed = true;
            self.not_empty.broadcast(g_io);
        }

        fn reset(self: *Self) void {
            self.mu.lockUncancelable(g_io);
            defer self.mu.unlock(g_io);
            self.head = 0;
            self.tail = 0;
            self.len = 0;
            self.closed = false;
        }
    };
}

const Chan = BoundedQueue(i32, CHAN_CAP);

const ProducerCtx = struct {
    queue: *Chan,
    send_count: usize,
    producer_id: usize,
};

const ConsumerCtx = struct {
    queue: *Chan,
    sum: *i64,
};

fn producerFn(ctx: ProducerCtx) void {
    for (0..ctx.send_count) |i| {
        const val: i32 = @truncate(@as(i64, @bitCast(@as(u64, (ctx.producer_id + 1) ^ (i << 1) ^ (i >> 16)))));
        ctx.queue.push(val);
    }
}

fn consumerFn(ctx: ConsumerCtx) void {
    var local: i64 = 0;
    while (ctx.queue.pop()) |v| {
        local += v;
    }
    @atomicStore(i64, ctx.sum, local, .release);
}

fn workShare(total: usize, idx: usize, workers: usize) usize {
    const base = total / workers;
    const rem = total % workers;
    return if (idx < rem) base + 1 else base;
}

fn runSharedCase(alloc: std.mem.Allocator, producers: usize, consumers: usize, messages: usize) !f64 {
    var queue = Chan{};

    const sums = try alloc.alloc(i64, consumers);
    defer alloc.free(sums);
    @memset(sums, 0);

    const start = monoNs();

    const consumer_threads = try alloc.alloc(std.Thread, consumers);
    defer alloc.free(consumer_threads);

    for (0..consumers) |ci| {
        consumer_threads[ci] = try std.Thread.spawn(.{}, consumerFn, .{ConsumerCtx{
            .queue = &queue,
            .sum = &sums[ci],
        }});
    }

    const producer_threads = try alloc.alloc(std.Thread, producers);
    defer alloc.free(producer_threads);

    for (0..producers) |pi| {
        producer_threads[pi] = try std.Thread.spawn(.{}, producerFn, .{ProducerCtx{
            .queue = &queue,
            .send_count = workShare(messages, pi, producers),
            .producer_id = pi,
        }});
    }

    for (0..producers) |pi| {
        producer_threads[pi].join();
    }
    queue.close();

    for (0..consumers) |ci| {
        consumer_threads[ci].join();
    }

    var total: i64 = 0;
    for (sums) |s| total += s;
    _ = @atomicRmw(i64, &sink, .Add, total, .monotonic);

    const elapsed = monoNs() - start;
    return @as(f64, @floatFromInt(elapsed)) / 1_000_000.0;
}

pub fn main() !void {
    const alloc = std.heap.c_allocator;

    // g_io services only the contended mutex/condvar paths (futex wait/wake).
    var threaded: Io.Threaded = .init(std.heap.page_allocator, .{});
    defer threaded.deinit();
    g_io = threaded.io();

    const messages = envIntOrDefault("CC_CONTENTION_ITERATIONS", DEFAULT_MESSAGES);
    const trials = envIntOrDefault("CC_CONTENTION_TRIALS", DEFAULT_TRIALS);
    const producers = envIntOrDefault("CC_CONTENTION_PRODUCERS", DEFAULT_PRODUCERS);
    const consumers = envIntOrDefault("CC_CONTENTION_CONSUMERS", DEFAULT_CONSUMERS);

    printf("=================================================================\n", .{});
    printf("ZIG SHARED CHANNEL CONTENTION\n", .{});
    printf("Messages: {d} | Trials: {d} | Contention: {d}x{d}\n", .{ messages, trials, producers, consumers });
    printf("=================================================================\n\n", .{});

    var best_baseline: f64 = std.math.floatMax(f64);
    var best_contention: f64 = std.math.floatMax(f64);

    for (1..trials + 1) |trial| {
        const baseline_ms = try runSharedCase(alloc, 1, 1, messages);
        const contention_ms = try runSharedCase(alloc, producers, consumers, messages);

        if (baseline_ms < best_baseline) best_baseline = baseline_ms;
        if (contention_ms < best_contention) best_contention = contention_ms;

        printf("  Trial {d}:  baseline={d:6.2} ms  contention={d:6.2} ms\n", .{ trial, baseline_ms, contention_ms });
    }

    const interference = (best_contention - best_baseline) / best_baseline * 100.0;
    const msgs_f: f64 = @floatFromInt(messages);

    printf("\n", .{});
    printf("  Best baseline:    {d:6.2} ms  ({d:8.0} msgs/sec)\n", .{ best_baseline, msgs_f / best_baseline * 1000.0 });
    printf("  Best contention:  {d:6.2} ms  ({d:8.0} msgs/sec)\n", .{ best_contention, msgs_f / best_contention * 1000.0 });
    printf("\n", .{});
    printf("Interference: {d:.2}%  (best-of-{d})\n", .{ interference, trials });
}
