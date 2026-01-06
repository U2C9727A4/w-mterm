const std = @import("std");

const mfs_message = struct {
    psize: u32,
    dsize: u32,
    op: u8,

    path: []const u8,
    data: []const u8
};

// Writes headers from an mfs_message struct into a given slice.
fn write_headers(slice: []u8, msg: mfs_message) !void {
    if (slice.len < 9) return error.SliceTooSmall;
    std.mem.writeInt(u32, slice[0..4], msg.psize, .little);
    std.mem.writeInt(u32, slice[4..8], msg.dsize, .little);
    slice[8] = msg.op;
}

// Readers headers from a slice into an mfs message.
fn read_headers(slice: []u8, msg: *mfs_message) !void {
    if (slice.len < 9) return error.SliceTooSmall;
    msg.psize = std.mem.readInt(u32, slice[0..4], .little);
    msg.dsize = std.mem.readInt(u32, slice[4..8], .little);
    msg.op = slice[8];
}

// Sends the given MFS message to stream
fn send_mfs_message(server: std.net.Stream, msg: mfs_message) !void {
    var headers: [9]u8 = undefined;
    try write_headers(&headers, msg);

    try server.writeAll(&headers);
    try server.writeAll(msg.path);
    try server.writeAll(msg.data);
}

// Gets (reads) an MFS message from the stream
// path and data fields of returned message is heap allocated.
fn get_mfs_message(server: std.net.Stream, allocator: std.mem.Allocator) !mfs_message {
    var return_message:mfs_message = undefined;

    var server_reader1 = server.reader(&.{});
    var server_reader = server_reader1.interface();


    var headers = try server_reader.readAlloc(allocator, 9);
    defer allocator.free(headers);
    try read_headers(headers[0..], &return_message);

    const path = try server_reader.readAlloc(allocator, return_message.psize);
    errdefer allocator.free(path);
    const data = try server_reader.readAlloc(allocator, return_message.dsize);
    errdefer allocator.free(data);

    return_message.data = data;
    return_message.path = path;
    return return_message;
}
// gets available bytes on file. POSIX SPECIFIC.
fn get_avail_bytes(file: std.fs.File) !u32 {
    var count: u32 = 0;
    const result =  std.posix.system.ioctl(file.handle, std.posix.T.FIONREAD, @intFromPtr(&count));
    if (result != 0) return error.IoctlFailed;
    return count;
}


var mcu: std.net.Stream = undefined;
const global_allocator = std.heap.page_allocator;
const gpath: []const u8 = "/etc/uart0";

fn rx_thread() !void {
        // Read a frame from charlie
        const read_request:mfs_message = .{
            .op = 1,
            .data = &.{},
            .dsize = 0,
            .path = gpath,
            .psize = 10,
        };
        try send_mfs_message(mcu, read_request);
        const response = try get_mfs_message(mcu, global_allocator);
        defer {
            global_allocator.free(response.path);
            global_allocator.free(response.data);
        }
        if (response.op != 0x81) std.process.abort();
        var stdout_writer = std.fs.File.stdout().writer(&.{}).interface;
        try stdout_writer.writeAll(response.data);


}

fn tx_thread() !void {
        // Lock mutex AFTER we read.
        var avail = try get_avail_bytes(std.fs.File.stdin());
        if (avail > 4096) avail = 4096;
        const buf = try global_allocator.alloc(u8, avail);
        defer global_allocator.free(buf);
        const bytes_read = try std.fs.File.stdin().read(buf);

        const write_request:mfs_message = .{
            .data = buf[0..bytes_read],
            .dsize = @truncate(bytes_read),
            .op = 2,
            .path = gpath,
            .psize = 10,
        };
        try send_mfs_message(mcu, write_request);
        const response = try get_mfs_message(mcu, global_allocator);
        defer {
            global_allocator.free(response.path);
            global_allocator.free(response.data);
        }
        if (response.op != 0x82) std.process.abort();

}

fn set_raw_mode(fd: std.posix.fd_t) !void {
    var cur_mode = try std.posix.tcgetattr(std.posix.STDIN_FILENO);
    cur_mode.lflag.ECHO = false;
    cur_mode.lflag.ICANON = false;
    try std.posix.tcsetattr(fd, .NOW, cur_mode);
}

fn disableNagle(mcur: std.net.Stream) !void {
    const one: c_int = 1;
    try std.posix.setsockopt(mcur.handle, std.posix.IPPROTO.TCP, std.posix.TCP.NODELAY, std.mem.asBytes(&one));
}

// argv: program "mcu address" "port"
pub fn main() !void {
    const args = try std.process.argsAlloc(global_allocator);
    defer std.process.argsFree(global_allocator, args);
    if (args.len < 3) std.debug.print("USAGE: compiny charlie_ip charlie_port", .{});

    const mcu_ip = args[1][0..args[1].len];
    const mcu_port = args[2][0..args[2].len];

    try set_raw_mode(std.fs.File.stdin().handle);

    mcu = try std.net.tcpConnectToHost(global_allocator, mcu_ip, try std.fmt.parseInt(u16, mcu_port, 10));
    try disableNagle(mcu);

    while (true) {
        try tx_thread();
        try rx_thread();
        std.Thread.sleep(10 * std.time.ns_per_ms);
    }
}
