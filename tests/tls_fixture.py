"""Local HTTPS checks with an ephemeral certificate; no external service/token."""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
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
        ]:
            subprocess.run([probe, f"{host}:{port}", path, mode], env=env, check=True, timeout=10)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
print("Local TLS trust, hostname, response/header, body-limit and cancellation checks passed")
