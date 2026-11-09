#!/usr/bin/env python3
"""
Cerelog ESP-EEG microSD Player (BETA)

Starts a small local web server and opens the viewer in your browser.
No extra packages needed: standard-library Python 3 only.

Usage:
    python3 usd_player.py                    # open the viewer, then drop/choose a file
    python3 usd_player.py REC_001.csv        # open the viewer with that file loaded

Press Ctrl+C in this terminal to stop.
"""

import argparse
import http.server
import os
import socketserver
import sys
import threading
import urllib.parse
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
VIEWER = os.path.join(HERE, "viewer.html")
PREFERRED_PORT = 8765


def make_handler(data_path):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            path = urllib.parse.urlparse(self.path).path
            if path in ("/", "/index.html"):
                self._send_file(VIEWER, "text/html; charset=utf-8")
            elif path == "/data" and data_path:
                self._send_file(data_path, "text/csv; charset=utf-8",
                                {"X-Filename": urllib.parse.quote(os.path.basename(data_path))})
            else:
                self.send_error(404)

        def _send_file(self, fp, ctype, extra=None):
            try:
                with open(fp, "rb") as f:
                    body = f.read()
            except OSError as e:
                self.send_error(500, str(e))
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass  # keep the terminal quiet

    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="Cerelog ESP-EEG microSD player (beta)")
    ap.add_argument("file", nargs="?", help="REC_xxx.csv from the microSD card (optional)")
    ap.add_argument("--port", type=int, default=PREFERRED_PORT)
    ap.add_argument("--no-browser", action="store_true", help="don't open a browser automatically")
    args = ap.parse_args()

    data_path = None
    if args.file:
        data_path = os.path.abspath(args.file)
        if not os.path.isfile(data_path):
            sys.exit(f"File not found: {args.file}")

    handler = make_handler(data_path)
    try:
        httpd = Server(("127.0.0.1", args.port), handler)
    except OSError:
        httpd = Server(("127.0.0.1", 0), handler)  # preferred port busy: pick any free one
    url = f"http://127.0.0.1:{httpd.server_address[1]}/"

    print("ESP-EEG microSD Player (beta)")
    print(f"  Viewer running at {url}")
    if data_path:
        print(f"  Loaded: {data_path}")
    print("  Press Ctrl+C to stop.")

    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
