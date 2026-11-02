"""mcwave Windows gateway: gives a USB-attached mcwave its clock and its network.

The firmware still does its own TLS. For every HTTPS connection it asks this program (OPEN) to open a
plain TCP connection to the real server and then relays the encrypted bytes, so this PC never sees a
password or token. No settings, no account: plug mcwave into USB and keep this program running.

Host->device bytes are numbered and acknowledged (go-back-N): the ESP32's UART has no hardware flow
control and drops bytes while flash is being written, so the gateway resends anything not acknowledged.
"""
import argparse
import json
import logging
import logging.handlers
import os
import socket
import struct
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # unit tests inject their own transport
    serial = None

import protocol as P

LOG = logging.getLogger("mcwave")
HOST_TIMEOUT_S = 12.0      # no frame from the device for this long: the session is over
PING_EVERY_S = 4.0
CHUNK = 1000
RESEND_AFTER_S = 0.25      # no acknowledgement progress for this long: resend what is unacknowledged
CLOSE_GIVE_UP_S = 15.0
ALLOWED_PORTS = (80, 443)  # the firmware only speaks HTTP(S); never relay anything else
VID, PID = 0x1A86, 0x55D3  # WCH CH343 on the mcwave driver board


class Connection:
    """One relayed TCP connection with a reliable host->device byte stream."""

    def __init__(self, session, conn_id, host, port):
        self.session, self.id, self.host, self.port = session, conn_id, host, port
        self.sock = None
        self.cond = threading.Condition()
        self.alive = True
        self.opened = False
        self.sent = 0            # bytes handed to the device so far (stream offset)
        self.acked = 0           # device has accepted this many bytes in order
        self.consumed = 0        # device has delivered this many bytes to its TLS socket
        self.unacked = []        # [(offset, bytes)] not yet accepted
        self.last_progress = time.monotonic()
        self.eof_total = None    # set once the server closed: total bytes the device must receive
        self.device_closed = False
        self.resends = 0
        threading.Thread(target=self._run, name="conn%d" % conn_id, daemon=True).start()

    def send_open_result(self, ok):
        self.session.send(P.OPEN_RESULT, self.id, bytes([1, 0] if ok else [0, 1]))

    def _send_data(self, offset, data):
        self.session.send(P.DATA_IN, self.id, struct.pack("<I", offset) + data)

    def _run(self):
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=10)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sock.settimeout(0.05)
        except OSError as exc:
            LOG.info("open %s:%s failed: %s", self.host, self.port, exc)
            self.send_open_result(False)
            self.session.drop(self.id)
            return
        self.opened = True
        self.send_open_result(True)
        LOG.info("open %s:%s ok", self.host, self.port)
        try:
            self._pump()
        except OSError as exc:
            LOG.info("socket error %s:%s: %s", self.host, self.port, exc)
        finally:
            LOG.info("closed %s:%s (to device %d bytes, %d resends)", self.host, self.port, self.sent, self.resends)
            self.alive = False
            self.close_socket()
            self.session.drop(self.id)

    def _pump(self):
        eof_at, last_close = None, 0.0
        while self.alive:
            now = time.monotonic()
            with self.cond:
                if self.unacked and now - self.last_progress > RESEND_AFTER_S:
                    pending = list(self.unacked)
                    self.last_progress = now
                else:
                    pending = []
                window = P.CONN_CREDIT - (self.sent - self.consumed)
                eof_total = self.eof_total
                done = eof_total is not None and self.device_closed
            if done:
                return
            for offset, data in pending:       # go-back-N
                self.resends += 1
                self._send_data(offset, data)
            if eof_total is not None:
                if eof_at is None:
                    eof_at = now
                if now - eof_at > CLOSE_GIVE_UP_S:
                    LOG.info("device never confirmed the close of %s:%s", self.host, self.port)
                    return
                if not self.unacked and now - last_close >= RESEND_AFTER_S:
                    self.session.send(P.CLOSE_IN, self.id, struct.pack("<I", eof_total))
                    last_close = now
                with self.cond:
                    self.cond.wait(0.05)
                continue
            if window <= 0:
                with self.cond:
                    self.cond.wait(0.05)
                continue
            try:
                data = self.sock.recv(min(window, CHUNK))
            except socket.timeout:
                continue
            if not data:
                with self.cond:
                    self.eof_total = self.sent
                continue
            with self.cond:
                offset = self.sent
                self.sent += len(data)
                self.unacked.append((offset, data))
                if len(self.unacked) == 1:
                    self.last_progress = time.monotonic()
            self._send_data(offset, data)

    def on_ack(self, rx, consumed, closed):
        with self.cond:
            if rx > self.acked:
                self.acked = rx
                self.last_progress = time.monotonic()
                self.unacked = [(o, d) for o, d in self.unacked if o + len(d) > rx]
            if consumed > self.consumed:
                self.consumed = consumed
            if closed:
                self.device_closed = True
            self.cond.notify_all()

    def close_socket(self):
        s, self.sock = self.sock, None
        if s:
            try:
                s.close()
            except OSError:
                pass

    def from_device(self, payload):
        s = self.sock
        if s:
            try:
                s.sendall(payload)
            except (OSError, ValueError):
                self.shutdown()

    def shutdown(self):
        self.alive = False
        with self.cond:
            self.cond.notify_all()
        s = self.sock
        if s:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Session:
    """One attached device. `ser` needs read(n), write(b) and a writable baudrate."""

    def __init__(self, ser, on_state=None):
        self.ser = ser
        self.on_state = on_state or (lambda *_: None)
        self.write_lock = threading.Lock()
        self.conns = {}
        self.conns_lock = threading.Lock()
        self.parser = P.Parser()
        self.device = ""
        self.last_rx = time.monotonic()
        self.logbuf = bytearray()

    def send(self, ftype, conn, payload=b""):
        data = P.encode(ftype, conn, payload)
        with self.write_lock:
            self.ser.write(data)

    def drop(self, conn_id):
        with self.conns_lock:
            self.conns.pop(conn_id, None)

    def _log_byte(self, b):
        if b in (10, 13):
            if self.logbuf:
                LOG.info("[device] %s", self.logbuf.decode("utf-8", "replace"))
                self.logbuf.clear()
        elif len(self.logbuf) < 400:
            self.logbuf.append(b)

    def _pump(self):
        """Read what is available (at most one port timeout); returns decoded frames."""
        frames = []
        waiting = getattr(self.ser, "in_waiting", 0)
        data = self.ser.read(min(waiting, 4096) if waiting else 1)
        if data:
            self.last_rx = time.monotonic()
            for ev in self.parser.feed(data):
                if ev[0] == "frame":
                    frames.append(ev[1:])
                else:
                    self._log_byte(ev[1])
        return frames

    def handshake(self, timeout=20.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for baud in (P.SLOW_BAUD, P.FAST_BAUD):
                self.ser.baudrate = baud
                self.parser = P.Parser()
                self.send(P.HELLO, 0, struct.pack("<II", int(time.time()), P.FAST_BAUD))
                end = time.monotonic() + 1.2
                while time.monotonic() < end:
                    for ftype, conn, payload in self._pump():
                        if ftype == P.HELLO_ACK and len(payload) >= 4:
                            agreed = struct.unpack("<I", payload[:4])[0]
                            self.device = payload[4:].decode("ascii", "replace")
                            self.ser.baudrate = agreed
                            self.parser = P.Parser()
                            self.last_rx = time.monotonic()
                            LOG.info("device %r attached at %d baud", self.device, agreed)
                            return True
        return False

    def run(self):
        if not self.handshake():
            return False
        self.on_state("connected", self.device)
        next_ping = 0.0
        try:
            while True:
                now = time.monotonic()
                if now - self.last_rx > HOST_TIMEOUT_S:
                    LOG.info("device silent for %.0fs; ending session", HOST_TIMEOUT_S)
                    return True
                if now >= next_ping:
                    self.send(P.PING, 0, struct.pack("<I", int(time.time())))
                    next_ping = now + PING_EVERY_S
                for ftype, conn, payload in self._pump():
                    self._handle(ftype, conn, payload)
        finally:
            with self.conns_lock:
                conns, self.conns = list(self.conns.values()), {}
            for c in conns:
                c.shutdown()
            self.on_state("waiting", "")

    def _handle(self, ftype, conn, payload):
        if ftype == P.OPEN and len(payload) >= 3:
            port = payload[0] | (payload[1] << 8)
            host = payload[2:].decode("ascii", "replace")
            if (ALLOWED_PORTS is not None and port not in ALLOWED_PORTS) or not host or any(c.isspace() for c in host):
                LOG.info("refused open %s:%s", host, port)
                self.send(P.OPEN_RESULT, conn, bytes([0, 2]))
                return
            with self.conns_lock:
                old = self.conns.get(conn)
                if old and old.alive:
                    # The device numbers every connection uniquely and repeats OPEN until it hears the answer.
                    if old.opened:
                        old.send_open_result(True)
                    return
                self.conns[conn] = Connection(self, conn, host, port)
        elif ftype in (P.DATA_OUT, P.CLOSE_OUT, P.CREDIT):
            with self.conns_lock:
                c = self.conns.get(conn)
            if not c:
                return
            if ftype == P.DATA_OUT:
                c.from_device(payload)
            elif ftype == P.CREDIT and len(payload) >= 9:
                rx, consumed, closed = struct.unpack("<IIB", payload[:9])
                c.on_ack(rx, consumed, closed)
            elif ftype == P.CLOSE_OUT:
                c.shutdown()
                self.drop(conn)
        # PONG / HELLO_ACK repeats only refresh last_rx (done in _pump)


def open_port(name):
    """Open without pulsing DTR/RTS: those lines reset the ESP32."""
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = P.SLOW_BAUD
    ser.timeout = 0.05
    ser.write_timeout = 5
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    return ser


def candidate_ports(forced=None):
    if forced:
        return [forced]
    return [p.device for p in serial.tools.list_ports.comports() if p.vid == VID and p.pid == PID]


class App:
    def __init__(self, forced_port=None, status_path=None):
        self.forced = forced_port
        self.status_path = status_path
        self.stop = threading.Event()
        self.state = ("waiting", "")

    def set_state(self, state, detail):
        self.state = (state, detail)
        if self.status_path:
            try:
                with open(self.status_path, "w", encoding="utf-8") as f:
                    json.dump({"state": state, "device": detail, "updated": time.strftime("%Y-%m-%d %H:%M:%S")}, f, ensure_ascii=False)
            except OSError:
                pass

    def run_forever(self):
        self.set_state("waiting", "")
        while not self.stop.is_set():
            ports = candidate_ports(self.forced)
            attached = False
            for name in ports:
                try:
                    ser = open_port(name)
                except Exception as exc:  # port busy or unplugged
                    LOG.debug("cannot open %s: %s", name, exc)
                    continue
                try:
                    LOG.info("probing %s", name)
                    attached = Session(ser, self.set_state).run() or attached
                except Exception:
                    LOG.exception("session on %s failed", name)
                finally:
                    try:
                        ser.close()
                    except Exception:
                        pass
                    self.set_state("waiting", "")
            self.stop.wait(1.0 if attached else 3.0)


def setup_logging(directory, console):
    os.makedirs(directory, exist_ok=True)
    handlers = [logging.handlers.RotatingFileHandler(os.path.join(directory, "gateway.log"), maxBytes=1_000_000, backupCount=3, encoding="utf-8")]
    if console:
        handlers.append(logging.StreamHandler(sys.stdout))
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", handlers=handlers)


def single_instance():
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", 49322))
    except OSError:
        return None
    return s


def main(argv=None):
    ap = argparse.ArgumentParser(description="mcwave USB gateway")
    ap.add_argument("--port", help="serial port (default: auto-detect the mcwave driver board)")
    ap.add_argument("--console", action="store_true", help="also log to the console")
    ap.add_argument("--dir", default=os.path.join(os.environ.get("LOCALAPPDATA", "."), "McwaveGateway"))
    args = ap.parse_args(argv)
    setup_logging(args.dir, args.console)
    guard = single_instance()
    if guard is None:
        LOG.info("another gateway is already running")
        return 0
    LOG.info("mcwave gateway started")
    App(args.port, os.path.join(args.dir, "status.json")).run_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
