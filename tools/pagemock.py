#!/usr/bin/env python3
"""Serve the REAL web page from main_s3.cpp against a mock camera.

fxdiff.py proves the shaders; this proves the page. It serves INDEX_HTML
byte-for-byte, answers the endpoints it polls with a still frame and empty JSON,
and appends one test script that drives the page's own functions and reports
back. That is how "effects don't work on my iPhone" gets reproduced: run it in
the iOS Simulator and read what the page itself says went wrong.

    python3 tools/pagemock.py                  # http://localhost:8766/
    xcrun simctl openurl booted "http://localhost:8766/?tag=ios&test=fx"
"""
import json, re, pathlib, urllib.parse, http.server, socketserver, sys
ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / ".fxcheck"
PORT = 8766

def index():
    t = (ROOT / "main_s3.cpp").read_text()
    a = t.index('INDEX_HTML[] = R"rawliteral(') + len('INDEX_HTML[] = R"rawliteral(')
    return t[a:t.index(')rawliteral"', a)]

TEST = (OUT / "pagetest.js").read_text() if (OUT / "pagetest.js").exists() else ""

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, ctype, body, extra=None):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        if u.path == "/":
            h = index().replace("</body>", "<script>\n" + (OUT / "pagetest.js").read_text() + "\n</script></body>")
            return self._send(200, "text/html; charset=utf-8", h.encode())
        if u.path in ("/jpg", "/photo", "/frame"):
            return self._send(200, "image/jpeg", (OUT / "test.jpg").read_bytes())
        if u.path == "/r":
            with open(OUT / "page_reports.jsonl", "a") as f: f.write(q.get("d", ["{}"])[0] + "\n")
            return self._send(204, "text/plain", b"")
        if u.path == "/log":
            with open(OUT / "page_log.txt", "a") as f: f.write((q.get("m", [""])[0]) + "\n")
            return self._send(200, "text/plain", b"ok")
        if u.path == "/status":
            return self._send(200, "application/json", json.dumps(
                {"night": False, "sd": True, "photos": 3, "res": 9, "cam": True, "heap": 150000,
                 "rec": False, "photoRes": 21, "lag": 0, "peers": 0}).encode())
        if u.path in ("/archive/list", "/presets", "/files", "/link/peers", "/net/scan"):
            return self._send(200, "application/json", b"[]" if u.path != "/link/peers" else b'{"ok":true,"peers":[]}')
        return self._send(200, "application/json", b'{"ok":true}')
    do_POST = do_GET

if __name__ == "__main__":
    (OUT / "page_reports.jsonl").write_text(""); (OUT / "page_log.txt").write_text("")
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    print("mock camera at http://0.0.0.0:%d/" % PORT, flush=True)
    with socketserver.ThreadingTCPServer(("0.0.0.0", PORT), H) as s: s.serve_forever()
