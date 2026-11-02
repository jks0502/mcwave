"""mcwave USB link framing (mirrors WeatherClock/UsbFrame.h).

[A5 5A][type][conn][len lo][len hi][payload][crc16 lo][crc16 hi]; CRC-16/CCITT-FALSE over type..payload.
Bytes outside a valid frame are the firmware's text log.
"""
import struct

MAGIC = b"\xa5\x5a"
MAX_PAYLOAD = 1024
CONN_CREDIT = 4096
FAST_BAUD, SLOW_BAUD = 921600, 115200

HELLO, PING, OPEN_RESULT, DATA_IN, CLOSE_IN = 0x01, 0x02, 0x11, 0x12, 0x13        # host -> device
HELLO_ACK, PONG, OPEN, DATA_OUT, CLOSE_OUT, CREDIT = 0x81, 0x82, 0x91, 0x92, 0x93, 0x94  # device -> host


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, conn: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    body = struct.pack("<BBH", ftype, conn, len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


class Parser:
    """Feed bytes; yields ('frame', type, conn, payload) and ('log', byte) events."""

    def __init__(self):
        self.state = 0
        self.held = bytearray()   # bytes of a candidate frame (after the magic)
        self.bad_frames = 0

    def feed(self, data: bytes):
        for b in data:
            yield from self._byte(b)

    def _byte(self, b):
        if self.state == 0:
            if b == 0xA5:
                self.state = 1
            else:
                yield ("log", b)
            return
        if self.state == 1:
            if b == 0x5A:
                self.state = 2
                self.held.clear()
            elif b == 0xA5:
                yield ("log", 0xA5)       # stay in state 1
            else:
                self.state = 0
                yield ("log", 0xA5)
                yield ("log", b)
            return
        self.held.append(b)
        h = self.held
        if len(h) < 4:
            return
        size = h[2] | (h[3] << 8)
        if size > MAX_PAYLOAD:
            self.bad_frames += 1
            self.state = 0
            return
        if len(h) < 4 + size + 2:
            return
        body = bytes(h[:4 + size])
        crc = h[4 + size] | (h[5 + size] << 8)
        self.state = 0
        if crc16(body) != crc:
            self.bad_frames += 1
            return
        yield ("frame", body[0], body[1], body[4:])
