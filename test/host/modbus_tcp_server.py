#!/usr/bin/env python3
"""A small Modbus TCP server, written independently of the firmware.

Its purpose is to disagree. The firmware's request builder and response
checker are exercised against this rather than against themselves, so a shared
misunderstanding of the protocol has somewhere to show up. It is also what the
board can be pointed at on the bench to try the TCP master for real.

  python3 modbus_tcp_server.py [port] [bind]

Binds to 127.0.0.1 by default, which is all the host test needs. To point the
board at it, bind to the bench interface explicitly -- it speaks to anyone who
connects and has no authentication:

  python3 modbus_tcp_server.py 15020 0.0.0.0
"""
import socket, socketserver, struct, sys, threading

# Register images, written out as raw bytes so the expected values in the test
# are not derived with the same arithmetic the code under test uses.
HOLDING = {
    0: 0x1234, 1: 0x5678,      # u32 ABCD = 305419896, CDAB-read = 0x56781234
    2: 0x1234,                 # u16 4660
    3: 0xFFFF,                 # s16 -1
    4: 0x4367, 5: 0xD835,      # float32 231.845…
    6: 0x8000,                 # s16 -32768
    7: 0xFFFF, 8: 0xFFFF,      # u32 max / s32 -1
}
INPUT = {
    0: 0x42C8, 1: 0x0000,      # float32 100.0
    2: 0xC2C8, 3: 0x0000,      # float32 -100.0
}

UNIT = 11

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        while True:
            hdr = self._recv(7)
            if not hdr:
                return
            tid, pid, ln, uid = struct.unpack(">HHHB", hdr)
            pdu = self._recv(ln - 1)
            if pdu is None:
                return
            rsp = self._serve(uid, pdu)
            if rsp is None:          # silence, like a device that is not there
                continue
            self.request.sendall(struct.pack(">HHHB", tid, 0, len(rsp) + 1, uid) + rsp)

    def _recv(self, n):
        b = b""
        while len(b) < n:
            c = self.request.recv(n - len(b))
            if not c:
                return None
            b += c
        return b

    def _serve(self, uid, pdu):
        if uid != UNIT:
            return None                       # not for us: say nothing
        if len(pdu) != 5:
            return bytes([pdu[0] | 0x80, 0x03])
        fc, addr, cnt = struct.unpack(">BHH", pdu)
        if fc not in (3, 4):
            return bytes([fc | 0x80, 0x01])
        if cnt < 1 or cnt > 125:
            return bytes([fc | 0x80, 0x03])
        src = HOLDING if fc == 3 else INPUT
        out = b""
        for r in range(addr, addr + cnt):
            if r not in src:
                return bytes([fc | 0x80, 0x02])
            out += struct.pack(">H", src[r])
        return bytes([fc, len(out)]) + out

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 15020
    bind = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    with Server((bind, port), Handler) as s:
        print("listening on %s:%d" % (bind, port), flush=True)
        s.serve_forever()
