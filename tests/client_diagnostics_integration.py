#!/usr/bin/env python3
"""Check production WebSocket diagnostics, including an open anonymous browser."""
import importlib.util
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

module_path = Path(__file__).resolve().parents[1] / 'ft8/tests/web_integration.py'
spec = importlib.util.spec_from_file_location('web_integration', module_path)
web = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web)


def login(browser, text):
    values = [4, *text.encode()]
    browser.send(struct.pack('<' + 'f' * len(values), *values))


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(('127.0.0.1', 9001))
    with tempfile.TemporaryFile() as log:
        server = subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=log, stderr=log, text=True)
        browsers = []
        try:
            for _ in range(100):
                try:
                    browsers.append(web.Browser({
                        'User-Agent': 'SDR diagnostics test/1.0',
                        'Origin': 'https://sdr.example.test',
                        'X-Forwarded-For': '203.0.113.42, 192.168.144.112',
                        'X-Real-IP': '203.0.113.42',
                        'Forwarded': 'for=203.0.113.42;proto=https',
                        'Cookie': 'secret-cookie-do-not-log',
                        'Authorization': 'Bearer secret-token-do-not-log',
                    }, '/receiver?secret-query-do-not-log'))
                    break
                except ConnectionRefusedError:
                    assert server.poll() is None
                    time.sleep(0.02)
            assert browsers
            browsers.append(web.Browser())
            for browser in browsers:
                browser.status(5357000)
            # Browser #1 behaves like an old page with an empty callsign.
            for _ in range(5):
                login(browsers[0], '   :   ')
            browsers[0].send(b'12345')
            browsers[0].send(b'12345')
            browsers[0].send(b'opaque-text-must-not-be-logged', opcode=1)
            browsers[0].send(struct.pack('<2f', 1, 40))

            # Browser #2 sends no application messages. Answer control pings
            # while waiting for the real 15-second anonymous diagnostic timer.
            end = time.monotonic() + 16.2
            while time.monotonic() < end:
                ready, _, _ = select.select([b.sock for b in browsers], [], [], 0.1)
                for browser in browsers:
                    if browser.sock not in ready:
                        continue
                    first, second = browser.exact(2)
                    length = second & 127
                    assert first & 15 == 9 and length < 126, (first, second)
                    browser.send(browser.exact(length), opcode=10)
            # A valid login stops the anonymous reports; a later malformed name
            # is escaped, and no password part is included in diagnostic output.
            login(browsers[0], 'DL1ABC:DL1ABC')
            login(browsers[0], 'DL1ABC:SECRET-NOT-FOR-LOGS')
            login(browsers[1], 'bad\nFAKE:bad\nFAKE')
            for browser in browsers:
                browser.send(struct.pack('!H', 1000) + b'test done', opcode=8)
                browser.sock.settimeout(2)
                while browser.sock.recv(4096):
                    pass
            server.stdin.write('quit\n'); server.stdin.flush()
            server.wait(timeout=3)
            log.seek(0)
            output = log.read().decode(errors='replace')
            print(output, end='')
            assert '[Client #1] connected state=awaiting-login sockets=1' in output
            assert 'peer="127.0.0.1"' in output
            assert 'x-forwarded-for="203.0.113.42, 192.168.144.112"' in output
            assert 'x-real-ip="203.0.113.42"' in output
            assert 'forwarded="for=203.0.113.42;proto=https"' in output
            assert 'path="/receiver"' in output
            assert 'origin="https://sdr.example.test"' in output
            assert 'user-agent="SDR diagnostics test/1.0"' in output
            assert output.count('login rejected reason=whitespace-only-callsign username="   "') == 1
            assert output.count('binary frame ignored reason=invalid-size bytes=5') == 1
            assert '[Client #1] text frame ignored bytes=30' in output
            assert '[Client #1] command received before valid login type=1' in output
            assert '[Client #1] still awaiting valid login' in output
            assert 'rx=9 binary=8 text=1 malformed=2 login_attempts=5' in output
            assert '[Client #2] still awaiting valid login' in output
            assert 'rx=0 binary=0 text=0 malformed=0 login_attempts=0 last_rx=none login_data=no-login-received' in output
            assert 'login rejected reason=callsign-mismatch username="DL1ABC"' in output
            assert 'username="bad\\nFAKE"' in output
            assert output.count('disconnected code=1000 reason="test done"') == 2
            for secret in ['secret-cookie-do-not-log', 'secret-token-do-not-log', 'secret-query-do-not-log',
                           'SECRET-NOT-FOR-LOGS', 'opaque-text-must-not-be-logged', '\nFAKE']:
                assert secret not in output, secret
            assert 'Client connected:' not in output and 'clients.size():' not in output
            assert server.returncode == 0
            print('WebSocket journal: headers, anonymous activity, login rejection reasons, close summary and bounded/escaped output passed.')
        finally:
            for browser in browsers:
                browser.close()
            if server.poll() is None:
                server.stdin.write('quit\n'); server.stdin.flush()
                try:
                    server.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    server.kill(); server.wait()


if __name__ == '__main__':
    main()
