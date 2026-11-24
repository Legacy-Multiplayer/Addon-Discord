"""Local HTTPS checks with an ephemeral certificate; no external service/token."""
import http.server
import base64
import hashlib
import struct
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def read_frame(self):
        header = self.rfile.read(2)
        if len(header) != 2:
            return None, b""
        opcode, length = header[0] & 15, header[1] & 127
        if length == 126:
            length = struct.unpack("!H", self.rfile.read(2))[0]
        if length > 1024:
            raise ValueError("Unexpected fixture frame size")
        mask = self.rfile.read(4) if header[1] & 128 else b""
        payload = self.rfile.read(length)
        if mask:
            payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        return opcode, payload

    def websocket(self):
        key = self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        accept = base64.b64encode(hashlib.sha1(key.encode("ascii")).digest()).decode("ascii")
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        # Fragmented text with an interleaved ping: the client must assemble
        # text and service control frames without exposing them as messages.
        self.wfile.write(bytes([1, 2]) + b"he" + bytes([137, 1]) + b"x" + bytes([128, 3]) + b"llo")
        pong = False
        while True:
            opcode, payload = self.read_frame()
            if opcode is None:
                return
            if opcode == 10 and payload == b"x":
                pong = True
            elif opcode == 1 and payload == b"client" and pong:
                self.wfile.write(bytes([129, 3]) + b"ack")
            elif opcode == 8:
                self.wfile.write(bytes([136, len(payload)]) + payload)
                # Complete TLS close_notify as well as the WebSocket close frame.
                self.connection.unwrap().close()
                return
            else:
                raise ValueError("Unexpected WebSocket fixture frame")

    def do_GET(self):
        if self.path == "/gateway":
            try:
                self.websocket()
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass
            return
        if self.path == "/stall":
            time.sleep(2)
        self.send_response(200)
        self.send_header("Content-Length", str(33 * 1024 * 1024 if self.path == "/large" else 2))
        self.send_header("X-Fixture", "yes")
        self.end_headers()
        if self.path != "/large":
            try:
                self.wfile.write(b"ok")
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass


probe, openssl = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    cert, key = folder / "cert.pem", folder / "key.pem"
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(cert), "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.socket = context.wrap_socket(server.socket, server_side=True)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    port = server.server_port
    trusted = dict(os.environ, SSL_CERT_FILE=str(cert))
    untrusted = dict(os.environ, SSL_CERT_FILE=str(folder / "missing.pem"), SSL_CERT_DIR=str(folder))
    try:
        for host, path, mode, env in [
            ("localhost", "/", "ok", trusted),
            ("localhost", "/", "fail", untrusted),
            ("127.0.0.1", "/", "fail", trusted),
            ("localhost", "/large", "fail", trusted),
            ("localhost", "/stall", "cancel", trusted),
            ("localhost", "/gateway", "websocket", trusted),
        ]:
            subprocess.run([probe, f"{host}:{port}", path, mode], env=env, check=True, timeout=10)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
print("Local TLS, WebSocket fragmentation/ping, cancellation, reconnect and close checks passed")
