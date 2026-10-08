#!/usr/bin/env python3
"""Serve the firmware's embedded web UI with small local API fixtures."""

from __future__ import annotations

import json
import re
import argparse
import mimetypes
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main_s3.cpp").read_text(encoding="utf-8")
MATCH = re.search(r'R"rawliteral\((<!DOCTYPE html>.*)</html>\)rawliteral";', SOURCE, re.S)
if not MATCH:
    raise RuntimeError("INDEX_HTML raw literal not found")
INDEX = (MATCH.group(1) + "</html>").encode()


# What the fake camera remembers between requests, so the page's own settings round-trip.
STATE = {"viewrot": 1, "apCustom": False, "apPass": "wuwuwuwu", "wb": 0, "sat": 0}
TEST_JPG = os.environ.get("WUW_TEST_JPG")      # a frame to serve from /jpg instead of the skin art


class Handler(BaseHTTPRequestHandler):
    def send_bytes(self, body: bytes, content_type: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value) -> None:
        self.send_bytes(json.dumps(value).encode(), "application/json")

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/":
            self.send_bytes(INDEX, "text/html; charset=utf-8")
        elif path in ("/ritual-wuw", "/ritual-wuw/"):
            self.send_bytes((ROOT / "site" / "ritual-wuw" / "index.html").read_bytes(),
                            "text/html; charset=utf-8")
        elif path in ("/games", "/games/"):
            self.send_bytes((ROOT / "site" / "game-preview" / "index.html").read_bytes(),
                            "text/html; charset=utf-8")
        elif path.startswith("/games/"):
            base = (ROOT / "site" / "game-preview").resolve()
            file = (base / path[len("/games/"):]).resolve()
            if not file.is_relative_to(base) or not file.is_file():
                self.send_error(404)
                return
            self.send_bytes(file.read_bytes(),
                            mimetypes.guess_type(file.name)[0] or "application/octet-stream")
        elif path.startswith("/ui-art/"):
            name = Path(path).name
            file = ROOT / "assets" / f"wuw-skin-{name}"
            self.send_bytes(file.read_bytes(), "image/jpeg")
        elif path in ("/jpg", "/archive/img"):
            frame = Path(TEST_JPG) if TEST_JPG else ROOT / "assets" / "wuw-skin-nacre.jpg"
            self.send_bytes(frame.read_bytes(), "image/jpeg")
        elif path == "/status":
            self.send_json({"night": False, "sd": True, "photos": 12, "res": 9,
                            "photoRes": 21, "streamQ": 12, "photoQ": 4,
                            "bright": 0, "aeLevel": 0, "hmirror": True, "vflip": False,
                            "viewrot": STATE["viewrot"], "apCustom": STATE["apCustom"],
                            "wb": STATE["wb"], "sat": STATE["sat"],
                            "cam": True, "rec": False, "build": "preview"})
        elif path == "/set":
            query = dict(pair.split("=", 1) for pair in urlparse(self.path).query.split("&") if "=" in pair)
            if "viewrot" in query:
                STATE["viewrot"] = int(query["viewrot"]) & 3
            for key in ("wb", "sat"):
                if key in query:
                    STATE[key] = int(query[key])
            self.send_json({"ok": True})
        elif path == "/ap":
            self.send_json({"ok": True, "custom": STATE["apCustom"], "ssid": "wuw", "min": 8, "max": 63})
        elif path == "/room":
            self.send_json({"exhibition": True, "visitors": 2, "photos": 12,
                            "lastImage": 12, "lastAuthor": "Mira", "session": "PASAR_S0012",
                            "who": ["Mira", "guest-101"], "you": "guest-101"})
        elif path == "/chat/status":
            self.send_json({"configured": True, "online": True, "status": "online", "room": "main"})
        elif path == "/chat/messages":
            self.send_json([{"username": "Mira", "message": "Light is folding across the frame."},
                            {"username": "WUW-01", "message": "Camera ready."}])
        elif path == "/net":
            self.send_json({"ok": True, "ssid": "Studio WiFi", "online": True, "ip": "192.168.1.42"})
        elif path == "/net/scan":
            self.send_json({"ok": True, "scanning": False, "nets": [
                {"ssid": "Studio WiFi", "rssi": -43, "secure": True},
                {"ssid": "PASAR Guest", "rssi": -67, "secure": False}]})
        elif path == "/name":
            self.send_json({"ok": True, "name": "WUW-01"})
        elif path == "/files":
            self.send_json({"ok": True, "files": [
                {"n": "PASAR_S0012/IMG_0012.jpg", "kb": 286, "kind": "jpg", "i": 12},
                {"n": "PASAR_S0012/IMG_0011.jpg", "kb": 274, "kind": "jpg", "i": 11},
                {"n": "VID_0003.mov", "kb": 4812, "kind": "video"}]})
        elif path == "/gallery":
            self.send_json({"gen": 0, "shots": []})
        elif path == "/archive/list":
            self.send_json([{"n": 11, "t": 0, "v": 0}, {"n": 12, "t": 1, "v": 0}])
        elif path == "/presets":
            self.send_json([])
        elif path in ("/sync", "/link/peers", "/pre/status"):
            self.send_json({"ok": True, "gen": 0, "shots": [], "who": [], "peers": []})
        else:
            self.send_json({"ok": True})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode() if length else ""
        if urlparse(self.path).path == "/ap/pass":
            if not self.headers.get("X-Wuw-Ap"):
                self.send_json({"ok": False, "err": "not allowed"})
                return
            current, _, wanted = body.partition("\n")
            if current != STATE["apPass"]:
                self.send_json({"ok": False, "err": "current password is wrong"})
            elif len(wanted) < 8:
                self.send_json({"ok": False, "err": "needs at least 8 characters"})
            else:
                STATE["apPass"], STATE["apCustom"] = wanted, True
                self.send_json({"ok": True, "custom": True, "applyMs": 1500})
            return
        self.send_json({"ok": True})

    def log_message(self, *_args) -> None:
        pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Preview the WUW CAM web interfaces")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    print(f"WUW CAM preview: http://127.0.0.1:{args.port}/games")
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
