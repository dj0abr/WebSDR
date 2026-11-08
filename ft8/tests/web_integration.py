#!/usr/bin/env python3
"""Exercise real two-client WebSocket broadcasts using only Python's stdlib."""
import base64
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time


class Browser:
    def __init__(self, headers=None, path='/'):
        self.sock = socket.create_connection(('127.0.0.1', 9001), timeout=5)
        self.sock.settimeout(5)
        self.buffer = b''
        key = base64.b64encode(os.urandom(16)).decode()
        extra_headers = ''.join(f'{name}: {value}\r\n' for name, value in (headers or {}).items())
        self.sock.sendall((f'GET {path} HTTP/1.1\r\nHost: localhost:9001\r\nUpgrade: websocket\r\n'
                           'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                           f'Sec-WebSocket-Key: {key}\r\n{extra_headers}\r\n').encode())
        while b'\r\n\r\n' not in self.buffer:
            self.buffer += self.sock.recv(8192)
        headers, self.buffer = self.buffer.split(b'\r\n\r\n', 1)
        assert b'101 Switching Protocols' in headers, headers

    def exact(self, size):
        while len(self.buffer) < size:
            chunk = self.sock.recv(max(8192, size-len(self.buffer)))
            assert chunk, 'Unexpected WebSocket EOF'
            self.buffer += chunk
        result, self.buffer = self.buffer[:size], self.buffer[size:]
        return result

    def send(self, payload, opcode=2):
        mask = os.urandom(4)
        assert len(payload) < 126
        encoded = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
        self.sock.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + encoded)

    def packet(self):
        while True:
            first, second = self.exact(2)
            assert first & 0x80 and not second & 0x80
            length = second & 127
            if length == 126:
                length, = struct.unpack('!H', self.exact(2))
            elif length == 127:
                length, = struct.unpack('!Q', self.exact(8))
            payload = self.exact(length)
            opcode = first & 15
            if opcode == 9:
                self.send(payload, 10)
                continue
            assert opcode == 2 and len(payload) == 4100, (opcode, len(payload))
            return payload

    def records(self, expected):
        records = []
        while len(records) < expected:
            payload = self.packet()
            floats = struct.unpack('<1025f', payload)
            if floats[0] != 7:
                continue
            chars = []
            for value in floats[1:]:
                if value == 0:
                    break
                assert value.is_integer() and 0 < value < 256
                chars.append(chr(int(value)))
            text = ''.join(chars)
            assert text.endswith('\n'), 'Incomplete FT8 record'
            for record in text.splitlines():
                if record.startswith('D\t'):
                    records.append(record)
        assert len(records) == expected
        return records

    def status(self, dial):
        expected = f'S\t{dial}\n'
        while True:
            floats = struct.unpack('<1025f', self.packet())
            if floats[0] == 7:
                text = ''.join(chr(int(v)) for v in floats[1:] if v)
                if text == expected:
                    return

    def legacy(self, kind):
        while True:
            packet = self.packet()
            values = struct.unpack('<1025f', packet)
            if values[0] == kind:
                assert values[1] == 42.5
                return packet

    def close(self):
        self.sock.close()


def main():
    executable = str(Path(sys.argv[1]).resolve())
    subprocess.run([executable, '--queue-test'], check=True, timeout=5)
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # Allow TIME_WAIT from the previous test.
        probe.bind(('127.0.0.1', 9001))  # Never interfere with a running receiver.
    with tempfile.TemporaryFile() as log:
        server = subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=log, stderr=log, text=True)
        browsers = []
        try:
            for _ in range(50):
                try:
                    browsers.append(Browser())
                    break
                except ConnectionRefusedError:
                    assert server.poll() is None, 'WebSocket test server exited'
                    time.sleep(0.02)
            assert browsers, 'Test server did not listen'
            browsers.append(Browser())
            for browser in browsers:
                browser.status(5357000)

            def command(text):
                server.stdin.write(text + '\n')
                server.stdin.flush()

            # More than 100 binary frames in one decode burst. This must not
            # overflow the shared 100-item audio/FFT transmission queue.
            command('emit 1800')
            first, second = (browser.records(1800) for browser in browsers)
            assert first == second
            assert first[0].endswith('CQ K0ABC FN42') and first[-1].endswith('CQ K1799ABC FN42')
            assert all(row.startswith('D\t1700000010\t5357000\t') for row in first)
            command('freq 7074000')
            for browser in browsers:
                browser.status(7074000)
            command('emit 1 5357000')  # A previous band's decoder completed late.
            assert browsers[0].records(1) == browsers[1].records(1)
            command('legacy')
            assert browsers[0].legacy(0) == browsers[1].legacy(0)
            browsers[0].send(b'12345')  # Invalid binary command must not corrupt memory.
            command('target')
            browsers[0].legacy(3)
            assert not browsers[1].buffer and not select.select([browsers[1].sock], [], [], 0.1)[0]
            browsers.append(Browser())
            browsers[-1].status(7074000)  # Current status is sent on every connection.
            command('freq 0')
            for browser in browsers:
                browser.status(0)
            print('FT8 WebSocket: identical broadcasts to two clients, 1800 decodes without queue loss,')
            print('band metadata, reconnect status, legacy broadcasts and targeted audio passed.')
        finally:
            for browser in browsers:
                browser.close()
            if server.poll() is None:
                server.stdin.write('quit\n')
                server.stdin.flush()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
            if server.returncode:
                log.seek(0)
                sys.stderr.write(log.read().decode(errors='replace'))


if __name__ == '__main__':
    main()
