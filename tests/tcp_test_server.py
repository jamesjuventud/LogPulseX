#!/usr/bin/env python3
"""Minimal TCP line-capture server for testing NetworkSink.

Usage: server.py <port> <output_file> [--drop-after-n N] [--refuse]

Accepts connections, writes every received line to output_file
(flushing immediately), and exits after receiving a "__STOP__" line or
30s of no new connection. With --drop-after-n, closes the connection
after N lines to simulate a mid-stream network failure (server keeps
listening for a reconnect). With --refuse, immediately closes any
accepted connection (simulating a dead/refusing peer).
"""
import socket
import sys

def main():
    port = int(sys.argv[1])
    outfile = sys.argv[2]
    drop_after_n = None
    refuse = False
    if "--drop-after-n" in sys.argv:
        drop_after_n = int(sys.argv[sys.argv.index("--drop-after-n") + 1])
    if "--refuse" in sys.argv:
        refuse = True

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    srv.settimeout(30)

    with open(outfile, "a") as out:
        while True:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                break
            if refuse:
                conn.close()
                continue
            conn.settimeout(10)
            buf = b""
            n = 0
            try:
                while True:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        text = line.decode(errors="replace")
                        if text == "__STOP__":
                            conn.close()
                            srv.close()
                            return
                        out.write(text + "\n")
                        out.flush()
                        n += 1
                        if drop_after_n is not None and n >= drop_after_n:
                            conn.close()
                            raise SystemExit
            except (socket.timeout, ConnectionResetError, BrokenPipeError, SystemExit):
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

if __name__ == "__main__":
    main()
