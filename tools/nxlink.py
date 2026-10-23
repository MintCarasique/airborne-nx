"""Python port of devkitPro's nxlink client (switchbrew/switch-tools nxlink.c).

Usage:
    python tools/nxlink.py <file.nro> [-a IP] [-p remote_path] [-s]
                           [--log FILE] [--timeout SEC] [-r RETRIES]
                           [--args "extra args"] [-- nro args...]

  -a IP         console IP/hostname; omitted -> UDP broadcast discovery
  -p PATH       remote path (default: the file name; trailing '/' appends it)
  -s            after upload, listen on TCP 28771 and print console stdio
  --log FILE    with -s, also append received stdio to FILE (flushed per chunk)
  --timeout SEC with -s, stop listening after SEC seconds (0 = no limit)

Wire protocol (all integers little-endian uint32, TCP port 28280):
  C->S  namelen, name bytes, filesize        S->C  int32 status (0 = ok;
        -1 create failed, -2 no space, -3 no memory)
  C->S  repeated {chunklen, zlib-stream bytes} (zlib default level, 16 KiB
        input reads / output buffers, one continuous zlib stream)
  S->C  int32 status after the stream ends
  C->S  cmdlen, then cmdlen bytes of NUL-terminated args
Discovery: UDP broadcast "nxboot" -> 28280, reply "bootnx" received on 28771.
With -s, "_NXLINK_=<pc ip>" is appended to the args.
"""
import argparse
import shlex
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

SERVER_PORT = 28280
CLIENT_PORT = 28771
CHUNK = 16 * 1024
CMDBUF = 3072


def log_err(msg):
    print(msg, file=sys.stderr)


def discover(retries):
    print("pinging switch")
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        rx.bind(("0.0.0.0", CLIENT_PORT))
    except OSError as e:
        log_err(f"bind receive socket: {e}")
        tx.close(); rx.close()
        return None
    rx.settimeout(0.15)
    found = None
    try:
        while retries > 0 and not found:
            try:
                tx.sendto(b"nxboot", ("255.255.255.255", SERVER_PORT))
            except OSError as e:
                log_err(f"sendto: {e}")
            try:
                data, addr = rx.recvfrom(256)
                if data.startswith(b"bootnx"):
                    found = addr[0]
                    break
            except socket.timeout:
                pass
            retries -= 1
    finally:
        tx.close(); rx.close()
    return found


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        part = sock.recv(n - len(buf))
        if not part:
            return None
        buf += part
    return buf


def recv_i32(sock):
    b = recv_exact(sock, 4)
    return None if b is None else struct.unpack("<i", b)[0]


def build_args(positional, extra, nxlink_host):
    """cmdbuf layout: uint32 LE length, then NUL-terminated args (max 3072)."""
    args = list(positional)
    if extra:
        args += shlex.split(extra)
    if nxlink_host:
        args.append(f"_NXLINK_={nxlink_host}")
    body = b""
    for a in args:
        enc = a.encode() + b"\0"
        if len(body) + len(enc) + 5 >= CMDBUF - 2:
            break
        body += enc
    return struct.pack("<I", len(body)) + body


def send_nro(addr, name, data, cmd):
    try:
        sock = socket.create_connection((addr, SERVER_PORT), timeout=10)
    except OSError as e:
        log_err(f"Connection to {addr} failed: {e}")
        return 1
    sock.settimeout(None)
    nm = name.encode()
    try:
        with sock:
            try:
                sock.sendall(struct.pack("<I", len(nm)) + nm + struct.pack("<I", len(data)))
            except OSError as e:
                log_err(f"Failed sending filename/length: {e}")
                return 1
            resp = recv_i32(sock)
            if resp is None:
                log_err("Invalid response")
                return 1
            if resp != 0:
                log_err({-1: "Failed to create file", -2: "Insufficient space",
                         -3: "Insufficient memory"}.get(resp, f"Unknown error {resp}"))
                return 1
            print(f"Sending {name}, {len(data)} bytes")
            comp = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION)
            total = blocks = 0

            def emit(buf):
                nonlocal total, blocks
                for i in range(0, len(buf), CHUNK):
                    piece = buf[i:i + CHUNK]
                    sock.sendall(struct.pack("<I", len(piece)) + piece)
                    total += len(piece)
                    blocks += 1
            try:
                for i in range(0, len(data), CHUNK):
                    emit(comp.compress(data[i:i + CHUNK]))
                emit(comp.flush())
            except OSError as e:
                log_err(f"Failed sending {name}: {e}")
                return 1
            pct = total * 100.0 / len(data) if data else 0.0
            print(f"{total} sent ({pct:.2f}%), {blocks} blocks")
            if recv_i32(sock) is None:
                log_err(f"Failed sending {name}")
                return 1
            try:
                sock.sendall(cmd)
            except OSError as e:
                log_err(f"Failed sending command line: {e}")
                return 1
            try:
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
    except OSError as e:
        log_err(f"socket error: {e}")
        return 1
    return 0


def serve(timeout, logpath):
    print("starting server")
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        ls.bind(("0.0.0.0", CLIENT_PORT))
        ls.listen(10)
    except OSError as e:
        log_err(f"bind/listen on {CLIENT_PORT}: {e}")
        ls.close()
        return 1
    print("server active ...")
    deadline = time.monotonic() + timeout if timeout else None
    logf = open(logpath, "ab") if logpath else None
    reason = "unknown"
    conn = None
    rc = 0

    def remaining():
        if deadline is None:
            return 1.0
        return max(0.0, min(1.0, deadline - time.monotonic()))

    try:
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                reason = f"timeout after {timeout}s"
                break
            ls.settimeout(remaining() or 0.01)
            try:
                conn, peer = ls.accept()
                print(f"console connected from {peer[0]}:{peer[1]}")
                break
            except socket.timeout:
                continue
        if conn:
            ls.close()
            reason = "connection closed by console"
            while True:
                if deadline is not None and time.monotonic() >= deadline:
                    reason = f"timeout after {timeout}s"
                    break
                conn.settimeout(remaining() or 0.01)
                try:
                    data = conn.recv(4096)
                except socket.timeout:
                    continue
                except OSError as e:
                    reason = f"recv error: {e}"
                    rc = 1
                    break
                if not data:
                    break
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                if logf:
                    logf.write(data)
                    logf.flush()
    except KeyboardInterrupt:
        reason = "interrupted (Ctrl+C)"
    finally:
        for s in (conn, ls):
            if s:
                try:
                    s.close()
                except OSError:
                    pass
        if logf:
            logf.close()
    print(f"\nexiting ... ({reason})")
    return rc


def main():
    ap = argparse.ArgumentParser(prog="nxlink", description="nxlink client (Python port)")
    ap.add_argument("nro", type=Path)
    ap.add_argument("nro_args", nargs="*", help="arguments passed to the NRO (after --)")
    ap.add_argument("-a", "--address")
    ap.add_argument("-r", "--retries", type=lambda s: int(s, 0), default=10)
    ap.add_argument("-p", "--path")
    ap.add_argument("--args", dest="extra_args")
    ap.add_argument("-s", "--server", action="store_true")
    ap.add_argument("--log", type=Path)
    ap.add_argument("--timeout", type=float, default=0)
    a = ap.parse_args()

    try:
        data = a.nro.read_bytes()
    except OSError as e:
        log_err(f"Failed to open {a.nro}: {e}")
        return 1
    name = a.path if a.path else a.nro.name
    if a.path and a.path.endswith("/"):
        name = a.path + a.nro.name

    if a.address is None:
        addr = discover(a.retries)
        if not addr:
            print("No response from Switch!")
            return 1
    else:
        try:
            addr = socket.getaddrinfo(a.address, None, socket.AF_INET)[0][4][0]
        except OSError:
            log_err("Invalid address")
            return 1

    host = None
    if a.server:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
                u.connect((addr, SERVER_PORT))
                host = u.getsockname()[0]
        except OSError as e:
            log_err(f"Cannot determine local IP towards {addr}: {e}")
            return 1
        print(f"stdio host: {host}")

    if send_nro(addr, name, data, build_args(a.nro_args, a.extra_args, host)):
        return 1
    return serve(a.timeout, a.log) if a.server else 0


if __name__ == "__main__":
    sys.exit(main())
