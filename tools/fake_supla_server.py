#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal fake Supla server for local end-to-end testing of zigbee2supla.

It accepts device connections (TLS or plain TCP), accepts every registration,
prints registered devices and channel value changes, answers pings and can
send relay commands. It is NOT a Supla server replacement.

Example:
  openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
      -keyout key.pem -out cert.pem
  tools/fake_supla_server.py --port 20160 --cert cert.pem --key key.pem

zigbee2supla config: "supla_server": "localhost", "supla_port": 20160,
"supla_security_level": 2
"""

import argparse
import socket
import ssl
import struct
import threading
import time

TAG = b"SUPLA"
HEADER = struct.Struct("<5sBIII")  # tag, version, rr_id, call_id, data_size

DCS_PING_SERVER = 40
SDC_PING_SERVER_RESULT = 50
SD_REGISTER_DEVICE_RESULT_B = 71
DS_REGISTER_DEVICE_G = 76
DS_CHANNEL_VALUE_CHANGED_C = 103
SD_CHANNEL_SET_VALUE = 110
DS_CHANNEL_SET_VALUE_RESULT = 120
DCS_SET_ACTIVITY_TIMEOUT = 210
SDC_SET_ACTIVITY_TIMEOUT_RESULT = 220

RESULTCODE_TRUE = 3
CHANNELTYPE_RELAY = 2900

REGISTER_HEADER_SIZE = 584
CHANNEL_E = struct.Struct("<BiiiqBI8sBB")  # 36 bytes
VALUE_C = struct.Struct("<BBI8s")
NEW_VALUE = struct.Struct("<iBI8s")
NEW_VALUE_RESULT = struct.Struct("<Bib")

lock = threading.Lock()


def log(msg):
    with lock:
        print(time.strftime("%H:%M:%S"), msg, flush=True)


def cstr(data):
    return data.split(b"\0", 1)[0].decode("utf-8", "replace")


class Connection:
    def __init__(self, sock, addr, args):
        self.sock = sock
        self.addr = addr
        self.args = args
        self.version = 25
        self.rr_id = 1
        self.name = "?"
        self.relays = []
        self.registered_at = None
        self.command_sent = False

    def send(self, call_id, payload):
        packet = HEADER.pack(TAG, self.version, self.rr_id, call_id,
                             len(payload)) + payload + TAG
        self.rr_id += 1
        self.sock.sendall(packet)

    def handle(self, call_id, data):
        if call_id == DS_REGISTER_DEVICE_G:
            email = cstr(data[0:256])
            guid = data[272:288].hex().upper()
            self.name = cstr(data[288:489])
            soft = cstr(data[489:510])
            count = data[583]
            channels = []
            for i in range(count):
                off = REGISTER_HEADER_SIZE + i * CHANNEL_E.size
                (number, ch_type, _funcs, default, _flags, offline, _validity,
                 value, _icon, _sub) = CHANNEL_E.unpack_from(data, off)
                channels.append((number, ch_type, default, offline,
                                 value.hex()))
                if ch_type == CHANNELTYPE_RELAY:
                    self.relays.append(number)
            log(f"REGISTER '{self.name}' email={email} guid={guid} "
                f"soft='{soft}' channels={channels}")
            self.registered_at = time.time()
            self.send(SD_REGISTER_DEVICE_RESULT_B,
                      struct.pack("<iBBBH", RESULTCODE_TRUE, 120, 29, 1, 0))
        elif call_id == DS_CHANNEL_VALUE_CHANGED_C:
            number, offline, _validity, value = VALUE_C.unpack(data)
            log(f"VALUE '{self.name}' ch={number} offline={offline} "
                f"value={value.hex()}")
        elif call_id == DS_CHANNEL_SET_VALUE_RESULT:
            number, sender, success = NEW_VALUE_RESULT.unpack(data)
            log(f"SET_RESULT '{self.name}' ch={number} sender={sender} "
                f"success={success}")
        elif call_id == DCS_PING_SERVER:
            self.send(SDC_PING_SERVER_RESULT, bytes(16))
        elif call_id == DCS_SET_ACTIVITY_TIMEOUT:
            self.send(SDC_SET_ACTIVITY_TIMEOUT_RESULT,
                      struct.pack("<BBB", data[0], 10, 240))
        else:
            log(f"call {call_id} from '{self.name}' ({len(data)} bytes)")

    def maybe_send_command(self):
        if (self.args.set_after is None or self.command_sent or
                not self.registered_at or not self.relays):
            return
        if time.time() - self.registered_at >= self.args.set_after:
            self.command_sent = True
            value = bytes([1]) + bytes(7)
            log(f"COMMAND '{self.name}' ch={self.relays[0]} -> ON")
            self.send(SD_CHANNEL_SET_VALUE,
                      NEW_VALUE.pack(42, self.relays[0], 0, value))

    def run(self):
        buf = b""
        self.sock.settimeout(0.2)
        try:
            while True:
                try:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                except (socket.timeout, ssl.SSLWantReadError):
                    pass
                while len(buf) >= HEADER.size:
                    tag, version, _rr, call_id, size = HEADER.unpack_from(buf)
                    if tag != TAG:
                        raise ValueError("bad begin tag")
                    total = HEADER.size + size + len(TAG)
                    if len(buf) < total:
                        break
                    if buf[HEADER.size + size:total] != TAG:
                        raise ValueError("bad end tag")
                    self.version = version
                    self.handle(call_id, buf[HEADER.size:HEADER.size + size])
                    buf = buf[total:]
                self.maybe_send_command()
        except Exception as e:  # noqa: BLE001 - test tool
            log(f"connection '{self.name}' error: {e}")
        log(f"DISCONNECT '{self.name}'")
        self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=20160)
    parser.add_argument("--cert", help="PEM certificate (TLS)")
    parser.add_argument("--key", help="PEM private key (TLS)")
    parser.add_argument("--set-after", type=float, default=None,
                        help="send ON to the first relay channel of each "
                             "device N seconds after registration")
    args = parser.parse_args()

    ctx = None
    if args.cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.cert, args.key)

    server = socket.create_server((args.host, args.port), reuse_port=False)
    log(f"listening on {args.host}:{args.port} "
        f"({'TLS' if ctx else 'plain TCP'})")
    while True:
        sock, addr = server.accept()
        if ctx:
            try:
                sock = ctx.wrap_socket(sock, server_side=True)
            except ssl.SSLError as e:
                log(f"TLS handshake failed: {e}")
                sock.close()
                continue
        threading.Thread(target=Connection(sock, addr, args).run,
                         daemon=True).start()


if __name__ == "__main__":
    main()
