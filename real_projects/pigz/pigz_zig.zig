// pigz_zig.zig -- Parallel gzip compression in idiomatic Zig
//
// Mirrors the architecture of pigz_go.go and pigz_idiomatic.ccs:
//   - Read 128KB blocks from input
//   - Compress each block concurrently (bounded by channel capacity)
//   - Write compressed blocks in submission order
//   - Uses system zlib via @cImport (fair benchmark comparison)

const std = @import("std");
const c = @cImport({
    @cInclude("zlib.h");
});

const BLOCK_SIZE: usize = 128 * 1024;
const CHAN_CAP: usize = 16;

const gzip_header = [_]u8{ 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03 };

const CompressedResult = struct {
    data: []const u8,
    buf: []u8,
    original_len: usize,
};

const Slot = struct {
    result: ?CompressedResult = null,
    ready: bool = false,
    mu: std.Thread.Mutex = .{},
    cv: std.Thread.Condition = .{},

    fn setDone(self: *Slot) void {
        self.mu.lock();
        defer self.mu.unlock();
        self.ready = true;
        self.cv.signal();
    }

    fn waitDone(self: *Slot) void {
        self.mu.lock();
        defer self.mu.unlock();
        while (!self.ready) self.cv.wait(&self.mu);
    }
};

fn BoundedQueue(comptime T: type, comptime cap: usize) type {
    return struct {
        buffer: [cap]T = undefined,
        head: usize = 0,
        tail: usize = 0,
        len: usize = 0,
        closed: bool = false,
        mu: std.Thread.Mutex = .{},
        not_full: std.Thread.Condition = .{},
        not_empty: std.Thread.Condition = .{},

        const Self = @This();

        fn push(self: *Self, item: T) void {
            self.mu.lock();
            defer self.mu.unlock();
            while (self.len == cap) self.not_full.wait(&self.mu);
            self.buffer[self.tail] = item;
            self.tail = (self.tail + 1) % cap;
            self.len += 1;
            self.not_empty.signal();
        }

        fn pop(self: *Self) ?T {
            self.mu.lock();
            defer self.mu.unlock();
            while (self.len == 0) {
                if (self.closed) return null;
                self.not_empty.wait(&self.mu);
            }
            const item = self.buffer[self.head];
            self.head = (self.head + 1) % cap;
            self.len -= 1;
            self.not_full.signal();
            return item;
        }

        fn close(self: *Self) void {
            self.mu.lock();
            defer self.mu.unlock();
            self.closed = true;
            self.not_empty.broadcast();
        }
    };
}

const SlotQueue = BoundedQueue(*Slot, CHAN_CAP);

var g_bytes_in: i64 = 0;
var g_bytes_out: i64 = 0;
var g_blocks_done: i32 = 0;

fn compressBlock(data: []const u8, alloc: std.mem.Allocator) ?CompressedResult {
    const max_out = data.len + (data.len >> 3) + 256;
    const buf = alloc.alloc(u8, max_out + 18) catch return null;

    @memcpy(buf[0..10], &gzip_header);

    const crc: u32 = @intCast(c.crc32(0, @ptrCast(data.ptr), @intCast(data.len)));

    var strm = std.mem.zeroes(c.z_stream);
    if (c.deflateInit2_(
        &strm,
        6,
        c.Z_DEFLATED,
        -15,
        8,
        c.Z_DEFAULT_STRATEGY,
        c.zlibVersion(),
        @as(c_int, @intCast(@sizeOf(c.z_stream))),
    ) != c.Z_OK) {
        alloc.free(buf);
        return null;
    }

    strm.next_in = @ptrCast(@constCast(data.ptr));
    strm.avail_in = @intCast(data.len);
    strm.next_out = @ptrCast(buf[10..].ptr);
    strm.avail_out = @intCast(max_out);

    const ret = c.deflate(&strm, c.Z_FINISH);
    _ = c.deflateEnd(&strm);
    if (ret != c.Z_STREAM_END) {
        alloc.free(buf);
        return null;
    }

    var pos: usize = 10 + @as(usize, @intCast(strm.total_out));

    std.mem.writeInt(u32, buf[pos..][0..4], crc, .little);
    pos += 4;
    std.mem.writeInt(u32, buf[pos..][0..4], @as(u32, @intCast(data.len & 0xFFFFFFFF)), .little);
    pos += 4;

    _ = @atomicRmw(i64, &g_bytes_in, .Add, @intCast(data.len), .monotonic);
    _ = @atomicRmw(i64, &g_bytes_out, .Add, @intCast(pos), .monotonic);
    _ = @atomicRmw(i32, &g_blocks_done, .Add, 1, .monotonic);

    return .{ .data = buf[0..pos], .buf = buf, .original_len = data.len };
}

fn worker(block_buf: []u8, data_len: usize, slot: *Slot, alloc: std.mem.Allocator) void {
    defer alloc.free(block_buf);
    slot.result = compressBlock(block_buf[0..data_len], alloc);
    slot.setDone();
}

fn consumerThread(queue: *SlotQueue, out_file: std.fs.File, alloc: std.mem.Allocator) void {
    while (queue.pop()) |slot| {
        slot.waitDone();
        if (slot.result) |result| {
            out_file.writeAll(result.data) catch {};
            alloc.free(result.buf);
        }
        alloc.destroy(slot);
    }
}

fn compressFile(in_path: []const u8, out_path: []const u8, alloc: std.mem.Allocator) !void {
    const in_file = try std.fs.cwd().openFile(in_path, .{});
    defer in_file.close();

    const out_file = try std.fs.cwd().createFile(out_path, .{});
    defer out_file.close();

    var queue: SlotQueue = .{};

    const con = try std.Thread.spawn(.{}, consumerThread, .{ &queue, out_file, alloc });

    while (true) {
        const buf = try alloc.alloc(u8, BLOCK_SIZE);
        const n = in_file.read(buf) catch {
            alloc.free(buf);
            break;
        };
        if (n == 0) {
            alloc.free(buf);
            break;
        }

        const slot = try alloc.create(Slot);
        slot.* = .{};
        queue.push(slot);

        const t = try std.Thread.spawn(.{}, worker, .{ buf, n, slot, alloc });
        t.detach();

        if (n < BLOCK_SIZE) break;
    }

    queue.close();
    con.join();
}

pub fn main() !void {
    const alloc = std.heap.c_allocator;
    const args = try std.process.argsAlloc(alloc);
    defer std.process.argsFree(alloc, args);

    if (args.len < 2) {
        var ebuf: [256]u8 = undefined;
        var ew = std.fs.File.stderr().writer(&ebuf);
        try ew.interface.print("Usage: {s} <file>\n", .{args[0]});
        try ew.interface.flush();
        std.process.exit(1);
    }

    const in_path = args[1];
    const out_path = try std.fmt.allocPrint(alloc, "{s}.gz", .{in_path});
    defer alloc.free(out_path);

    try compressFile(in_path, out_path, alloc);

    var obuf: [4096]u8 = undefined;
    var ow = std.fs.File.stdout().writer(&obuf);
    const out = &ow.interface;
    try out.print("Compressed {s} -> {s}\n", .{ in_path, out_path });
    try out.print("Stats: {d} bytes in, {d} bytes out, {d} blocks\n", .{
        @atomicLoad(i64, &g_bytes_in, .seq_cst),
        @atomicLoad(i64, &g_bytes_out, .seq_cst),
        @atomicLoad(i32, &g_blocks_done, .seq_cst),
    });
    try out.flush();
}
