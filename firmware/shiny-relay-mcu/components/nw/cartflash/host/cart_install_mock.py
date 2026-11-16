#!/usr/bin/env python3
"""cart_install_mock.py: the three device routes of "Send to my Chromatic", for a bench unit
(lane cartflash1). The real ones are shiny_server_rs (notes/GBNET_GATEWAY.md s12, lane cartui1):
  POST /link_device/cart_install         -> {"pending": false} | {"pending": true, id, title, url, size, crc32, sha256, ...}
  POST /link_device/cart_install/ack     <- {"id", "state", "detail"}   (downloading / resuming / installed / declined / failed)
  GET  /gbnet/v1/install/<id>/rom        Range, X-Total-Length
Usage: cart_install_mock.py --rom <cart image> [--gsid <game state id>] [--title T] [--port-file F] [--bind 0.0.0.0] [--id <32 hex>]
Any ROM from 32,768 to 2,097,152 bytes whose bytes from 2,031,616 on are FF (owner ruling 2026-10-11); use
make_test_rom.py for a synthetic one. Test knobs: --outage-at / --outage-s (the image route answers 503 for a
while, once: the resume test), --fail-acks N (the first N reports get 503: the report retry test).
The image is served to the LAN only for the length of the test. The Authorization header is
checked for presence only and is never printed or stored (it is the device token).
Every line it prints is one JSON event.
"""
import argparse, hashlib, http.server, json, re, socketserver, sys, time, uuid, zlib

ap = argparse.ArgumentParser()
ap.add_argument("--rom", required=True)
ap.add_argument("--gsid", default="", help="optional: sent as game_state_id (any ROM may be installed since 2026-10-11)")
ap.add_argument("--title", default="")
ap.add_argument("--bind", default="0.0.0.0")
ap.add_argument("--port", type=int, default=0)
ap.add_argument("--port-file", default="")
ap.add_argument("--id", default="", help="32 lowercase hex: keep the same install id across restarts (the resume test)")
ap.add_argument("--outage-at", type=int, default=0, help="bytes: once this much was served, refuse the image (503) for --outage-s seconds, once (the resume test)")
ap.add_argument("--outage-s", type=float, default=90.0)
ap.add_argument("--fail-acks", type=int, default=0, help="answer the first N reports with 503 (the report retry test)")
ap.add_argument("--connections", action="store_true", help="count TCP connections on the image route (keep-alive check)")
ap.add_argument("--absolute-url", action="store_true", help="hand out the URL absolute (http://host:port/gbnet/v1/...)")
a = ap.parse_args()

rom = open(a.rom, "rb").read()
WRITABLE = 2031616
if not 32768 <= len(rom) <= 2097152 or any(b != 0xFF for b in rom[WRITABLE:]):
    sys.exit("the image must be 32,768 to 2,097,152 bytes, and every byte from 2,031,616 on must be FF (the backend's rule)")
title = a.title or rom[0x134:0x143].split(b"\0")[0].decode("ascii", "replace").strip() or "SHINY GEN GAME"
rec = {"id": a.id or uuid.uuid4().hex, "state": "pending", "detail": "", "size": len(rom),
       "crc32": "%08x" % zlib.crc32(rom), "sha256": hashlib.sha256(rom).hexdigest()}
stats = {"rom_gets": 0, "rom_bytes": 0, "acks": 0, "conns": set(), "outage_until": 0.0, "outage_done": False}
LIVE = ("pending", "downloading", "installing", "resuming")


def ev(**k):
    k["t"] = time.strftime("%H:%M:%S")
    print(json.dumps(k), flush=True)


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *x):  # never the request line's headers
        pass

    def send(self, code, body=b"", ctype="application/json", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def authed(self):
        v = self.headers.get("Authorization", "")
        return v.startswith("Bearer ") or v.startswith("Device ")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(n) if n else b""
        if not self.authed():
            return self.send(401, b'{"error":"Missing token"}')
        if self.path == "/link_device/cart_install":
            if rec["state"] in LIVE:
                host = self.headers.get("Host", "")
                url = f"install/{rec['id']}/rom"
                if a.absolute_url:
                    url = f"http://{host}/gbnet/v1/{url}"
                out = {"pending": True, "id": rec["id"], "game_state_id": a.gsid, "title": title, "url": url,
                       "size": rec["size"], "crc32": rec["crc32"], "sha256": rec["sha256"], "state": rec["state"], "expires_in": 86000}
            else:
                out = {"pending": False}
            ev(ev="poll", pending=out["pending"])
            return self.send(200, json.dumps(out).encode())
        if self.path == "/link_device/cart_install/ack":
            try:
                j = json.loads(body or b"{}")
            except ValueError:
                return self.send(400, b"{}")
            if j.get("id") != rec["id"] or rec["state"] not in LIVE:
                ev(ev="ack_refused", id=j.get("id"), state=j.get("state"))
                return self.send(404, b'{"error":"no such install"}')
            stats["acks"] += 1
            if stats["acks"] <= a.fail_acks:
                ev(ev="ack_503_on_purpose", n=stats["acks"], state=j.get("state"))
                return self.send(503, b"{}")
            if j.get("state") not in ("downloading", "installing", "resuming", "installed", "declined", "failed"):
                return self.send(400, b"{}")
            rec["state"], rec["detail"] = j["state"], str(j.get("detail", ""))[:80]
            ev(ev="ack", state=rec["state"], detail=rec["detail"], rom_gets=stats["rom_gets"], rom_bytes=stats["rom_bytes"], rom_connections=len(stats["conns"]))
            return self.send(200, json.dumps({"id": rec["id"], "state": rec["state"]}).encode())
        self.send(404, b"{}")

    def do_GET(self):
        m = re.fullmatch(r"/gbnet/v1/install/([0-9a-f]{32})/rom", self.path)
        if not m:
            return self.send(404, b"", "application/octet-stream")
        if not self.authed():
            return self.send(401, b"", "application/octet-stream")
        if m.group(1) != rec["id"] or rec["state"] not in LIVE:
            return self.send(404, b"", "application/octet-stream")
        total = len(rom)
        r = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if not r:
            body = rom[:4086]
            return self.send(200, body, "application/octet-stream", {"X-Total-Length": str(total)})
        s = int(r.group(1))
        e = min(int(r.group(2)) if r.group(2) else total - 1, total - 1, s + 4085)
        if s >= total:
            return self.send(416, b"", "application/octet-stream", {"Content-Range": f"bytes */{total}", "X-Total-Length": str(total)})
        if a.outage_at and not stats["outage_done"] and stats["rom_bytes"] >= a.outage_at:
            stats["outage_done"] = True
            stats["outage_until"] = time.time() + a.outage_s
            ev(ev="outage_begins", at=s, seconds=a.outage_s)
        if time.time() < stats["outage_until"]:
            return self.send(503, b"", "application/octet-stream")
        stats["conns"].add(self.client_address)
        stats["rom_gets"] += 1
        stats["rom_bytes"] += e - s + 1
        if stats["rom_gets"] % 64 == 0:
            ev(ev="progress", rom_gets=stats["rom_gets"], at=s)
        self.send(206, rom[s:e + 1], "application/octet-stream",
                  {"Content-Range": f"bytes {s}-{e}/{total}", "X-Total-Length": str(total)})


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


srv = S((a.bind, a.port), H)
port = srv.server_address[1]
if a.port_file:
    open(a.port_file, "w").write(str(port))
ev(ev="ready", port=port, id=rec["id"], size=rec["size"], crc32=rec["crc32"], sha256=rec["sha256"], title=title)
srv.serve_forever()
