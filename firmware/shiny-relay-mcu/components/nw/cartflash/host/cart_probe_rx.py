#!/usr/bin/env python3
"""cart_probe_rx.py: the owner's-Mac end of the READ-ONLY cart probe (lane cartflash1).

The Chromatic's ESP32 (stock 0.17.0 network window service) forwards the probe's GET/POST
calls to the gateway URL set with `nw url`; for the probe that URL is THIS program on the
owner's LAN, never a Shiny Gen server. It:

  GET  cp/hello      -> "CARTPROBE-LOCAL1" (the probe reads nothing from the cart without it)
  GET  cp/probe.bin  -> the probe program (sm83/build/cartprobe.bin; --probe to choose)
  POST cp/h          <- the cart header; -> what to dump (mapper, ROM banks, RAM banks, options)
  POST cp/r          <- 1 KiB chunks (ROM 'R', save RAM 'S', bank-0 holes 'Z'), 16-bit sums
  POST cp/x          <- fingerprints of banks past the declared size (--extended)
  POST cp/i          <- flash autoselect / CFI bytes (only a CP_FLASHID probe, only with
                        --allow-flash-id: OWNER APPROVAL, it writes command bytes to the cart)
  POST cp/e          <- the probe's end report; the dump is written and checked

The dump is the owner's backup of a commercial game: it is written ONLY to --out (default
~/ChromaticCartBackups/<time>), which must not be inside a git work tree, and it is never
sent anywhere. The device token the ESP32 adds (Authorization header) is never logged.

  python3 cart_probe_rx.py [--host 0.0.0.0] [--port 8791] [--out DIR] [--probe FILE]
        [--rom-banks N] [--ram-banks N --ram-kb K] [--extended [--ext-banks N]]
        [--allow-flash-id] [--unmap] [--port-file F] [--exit-on-end] [--mimic-prod]
"""
import argparse
import binascii
import datetime
import hashlib
import json
import os
import re
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROBE_GAME_ID = b"SHGCARTPROBE0001".hex()
MAGIC = b"CARTPROBE-LOCAL1"
HERE = os.path.dirname(os.path.abspath(__file__))
KNOWN_CHIPS = {  # facts from CART_FLASH_DESIGN.md (ModRetro CLI table + FlashGBX profiles)
    (0xBF, 0xC8): "SST39VF1681 (2 MiB)",
    (0xBF, 0xC9): "SST39VF1682 (2 MiB)",
    (0x9D, 0x9D, 0x7E, 0x7E): "IS29GL032 (4 MiB)",
    (0x01, 0x01, 0x53, 0x53): "S29JL032J (4 MiB, per FlashGBX profile)",
}
LOGO = bytes.fromhex("CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999"
                     "BBBB67636E0EECCCDDDC999FBBB9333E")


def in_git_tree(path):
    p = os.path.abspath(path)
    while True:
        if os.path.exists(os.path.join(p, ".git")):
            return p
        q = os.path.dirname(p)
        if q == p:
            return None
        p = q


class State:
    def __init__(self, a):
        self.a = a
        self.lock = threading.Lock()
        self.hdr = None
        self.plan = None
        self.rom = bytearray()
        self.ram = bytearray()
        self.have = set()
        self.bad_sums = 0
        self.rereads = 0
        self.unstable = 0
        self.ext = []
        self.ids = []
        self.ram_unstable = set()      # save RAM KiB indexes that never read the same twice
        self.ram_unstable_chunks = set()
        self.ramdiag = None
        self.end = None
        self.done = threading.Event()

    def log(self, **kw):
        kw["t"] = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(json.dumps(kw), flush=True)


def decide(st, hdr, flags):
    a = st.a
    t, rs, ras = hdr[0x47], hdr[0x48], hdr[0x49]
    if 0x19 <= t <= 0x1E:
        mbc = 5
    elif 0x01 <= t <= 0x03:
        mbc = 1
    elif 0x0F <= t <= 0x13:
        mbc = 3
    elif t in (0x00, 0x08, 0x09):
        mbc = 0
    else:
        mbc = None
    rom_banks = a.rom_banks or (2 << rs if rs <= 8 else 0)
    ram_kb = {0: 0, 1: 2, 2: 8, 3: 8, 4: 8, 5: 8}.get(ras, 0)
    ram_banks = {0: 0, 1: 1, 2: 1, 3: 4, 4: 16, 5: 8}.get(ras, 0)
    if a.ram_banks is not None:
        ram_banks, ram_kb = a.ram_banks, a.ram_kb
    opts = 0
    if a.extended and mbc:
        opts |= 1
    if a.allow_flash_id and (flags & 1):
        opts |= 2
    if a.unmap and mbc != 5:
        opts |= 4
    ext = a.ext_banks if a.extended else rom_banks
    go = 1
    why = []
    if mbc is None:
        go, why = 0, why + ["cart type 0x%02X: no plan for this mapper" % t]
    if mbc == 0:
        rom_banks = 2
    if rom_banks < 1 or rom_banks > 512:
        go, why = 0, why + ["rom banks %d" % rom_banks]
    if hdr[4:4 + 48] != LOGO:
        why.append("logo does not match (cart not seated? header unreadable?)")
    return dict(go=go, mbc=mbc or 0, rom_banks=rom_banks, ram_banks=ram_banks, ram_kb=ram_kb,
                opts=opts, ext_banks=max(ext, rom_banks), why=why)


def handle(st, method, p, data, client="usb"):
    """One probe call (path below content/): (http code, body). Shared by HTTP and USB."""
    with st.lock:
        if method == "GET" and p == "cp/hello":
            st.log(ev="hello", client=client)
            return (200, MAGIC)
        if method == "GET" and p == "cp/probe.bin":
            prog = open(st.a.probe, "rb").read()
            st.log(ev="probe_served", file=os.path.basename(st.a.probe), bytes=len(prog),
                   sha256=hashlib.sha256(prog).hexdigest())
            return (200, prog)
        if method != "POST":
            return (404, b"")
        if p == "cp/h":
            ver, flags, hdr = data[0], data[1], data[2:2 + 80]
            st.hdr = hdr
            st.plan = decide(st, hdr, flags)
            pl = st.plan
            st.rom = bytearray(b"\xff" * (pl["rom_banks"] * 0x4000))
            st.ram = bytearray(b"\xff" * (pl["ram_banks"] * pl["ram_kb"] * 1024))
            st.log(ev="header", probe_ver=ver, flashid_build=bool(flags & 1),
                   title=hdr[0x34:0x44].split(b"\0")[0].decode("latin-1"),
                   cgb=hdr[0x43], cart_type=hdr[0x47], rom_size=hdr[0x48], ram_size=hdr[0x49],
                   plan=pl)
            body = struct.pack("<BBHBBBH", pl["go"], pl["mbc"], pl["rom_banks"], pl["ram_banks"],
                               pl["ram_kb"], pl["opts"], pl["ext_banks"])
            return (200, body)
        if p == "cp/r":
            kind, bank, off, ln, s16, rr = struct.unpack("<BHHHHB", data[:10])
            chunk = data[10:10 + ln]
            if len(chunk) != ln or (sum(chunk) & 0xFFFF) != s16:
                st.bad_sums += 1
                st.log(ev="bad_chunk", kind=chr(kind), bank=bank, off=off)
                return (400, b"")
            if rr == 0xFF:
                st.unstable += 1
                if kind == ord("S"):
                    st.ram_unstable_chunks.add((bank, off))
            elif rr:
                st.rereads += rr
            if kind in (ord("R"), ord("Z")):
                base = bank * 0x4000 + off
                st.rom[base:base + ln] = chunk
            elif kind == ord("S"):
                base = bank * st.plan["ram_kb"] * 1024 + off
                if rr == 0xFF:         # never the same twice: floating, NOT a backup
                    for k in range(base // 1024, (base + ln + 1023) // 1024):
                        st.ram_unstable.add(k)
                    chunk = b"\xff" * ln
                st.ram[base:base + ln] = chunk
            st.have.add((kind, bank, off))
            if kind == ord("S") or os.environ.get("CP_RX_DEBUG"):
                st.log(ev="chunk", kind=chr(kind), bank=bank, off=off, rr=rr)
            if off == 0 and kind == ord("R") and bank % 8 == 0:
                st.log(ev="progress", bank=bank, of=st.plan["rom_banks"])
            return (200, b"OK")
        if p == "cp/x":
            first, cnt = struct.unpack("<HB", data[:3])
            for i in range(cnt):
                e = data[3 + 18 * i:3 + 18 * (i + 1)]
                s16 = struct.unpack("<H", e[:2])[0]
                st.ext.append(dict(bank=first + i, sum=s16, head=e[2:].hex()))
            return (200, b"OK")
        if p == "cp/i":
            var, idb, cfi, ok = data[0], data[1:17], data[17:145], data[145]
            four, two = tuple(idb[:4]), tuple(idb[:2])
            st.ids.append(dict(variant=var, id=idb.hex(), cfi=cfi.hex(), reset_ok=ok,
                               match=KNOWN_CHIPS.get(four) or KNOWN_CHIPS.get(two)))
            st.log(ev="flash_id", **st.ids[-1])
            return (200, b"OK")
        if p == "cp/d":
            d = [data[16 * i:16 * i + 16] for i in range(6)]
            v = dict(
                a400_fresh=d[0].hex(), a400_fresh_again=d[1].hex(), a000_after_0s5=d[2].hex(),
                a400_after_1s=d[3].hex(), a000_reselected=d[4].hex(), a400_reselected=d[5].hex(),
                a400_stable_right_after_select=d[0] == d[1] == d[5],
                late_reads_differ_from_reselected=(d[2] != d[4]) or (d[3] != d[5]))
            v["reading"] = ("TIME: reads float some time after RAM enable / bank select (the probe now re-selects before every read)"
                            if v["late_reads_differ_from_reselected"] and v["a400_stable_right_after_select"] else
                            "ADDRESS: 0xA400+ floats even right after select (no memory mapped there)"
                            if not v["a400_stable_right_after_select"] else "no difference seen")
            st.ramdiag = v
            st.log(ev="ram_diag", **v)
            return (200, b"OK")
        if p == "cp/e":
            f = struct.unpack("<BHHHHHBB", data[:13])
            st.end = dict(zip(("result", "chunks", "rereads", "bad", "retries", "gsum", "hsum", "bank0"), f))
            write_out(st)
            st.done.set()
            return (200, b"OK")
    return (404, b"")


class H(BaseHTTPRequestHandler):
    st = None
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def reply(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        return self.rfile.read(n) if n else b""

    def route(self, method):
        st = self.st
        m = re.match(r"^/gbnet/v1/games/([0-9a-f]{32})/content/(.*)$", self.path.split("?")[0])
        if st.a.mimic_prod or not m or m.group(1) != PROBE_GAME_ID:
            st.log(ev="ignored", method=method, path=self.path[:120])
            return self.reply(404)
        data = self.body() if method == "POST" else b""
        code, body = handle(st, method, m.group(2), data, self.client_address[0])
        return self.reply(code, body)

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")


def write_out(st):
    a, pl, hdr = st.a, st.plan, st.hdr
    os.makedirs(a.out, exist_ok=True)
    rom = bytes(st.rom)
    title = hdr[0x34:0x44].split(b"\0")[0].decode("latin-1").strip() or "cart"
    stem = re.sub(r"[^A-Za-z0-9_-]+", "_", title)
    rp = os.path.join(a.out, stem + ".gbc")
    open(rp, "wb").write(rom)
    ram_kib = len(st.ram) // 1024
    ram_bad = sorted(st.ram_unstable)
    ram_ok = bool(st.ram) and not ram_bad
    if st.ram:
        # an unstable region is never a backup: a .sav only when every KiB read the same twice
        open(os.path.join(a.out, stem + (".sav" if ram_ok else ".partial.sav")), "wb").write(bytes(st.ram))
    decl = (0x8000 << hdr[0x48]) if hdr[0x48] <= 8 else len(rom)
    g_decl = (sum(rom[:decl]) - rom[0x14E] - rom[0x14F]) & 0xFFFF if len(rom) > 0x150 else 0   # what the header's checksum covers
    g = (sum(rom) - rom[0x14E] - rom[0x14F]) & 0xFFFF if len(rom) > 0x150 else 0           # what the GB summed (the whole dump)
    hsum = 0
    for b in hdr[0x34:0x4D]:
        hsum = (hsum - b - 1) & 0xFF
    want_rom = pl["rom_banks"] * 16 * 1024 // 1024
    got_rom = sum(1 for k in st.have if k[0] in (ord("R"), ord("Z")))
    holes_missing = st.end["bank0"] == 2
    rep = dict(
        title=title, out=a.out, rom_file=rp, rom_bytes=len(rom),
        rom_crc32="%08x" % (binascii.crc32(rom) & 0xFFFFFFFF), rom_sha256=hashlib.sha256(rom).hexdigest(),
        ram_bytes=len(st.ram), ram_sha256=hashlib.sha256(bytes(st.ram)).hexdigest() if st.ram else None,
        chunks_rom="%d/%d" % (got_rom, want_rom), bad_sums=st.bad_sums, rereads=st.rereads, unstable=st.unstable,
        header_checksum=dict(header=hdr[0x4D], computed_here=hsum, computed_on_gb=st.end["hsum"]),
        global_checksum=dict(header="%04x" % ((hdr[0x4E] << 8) | hdr[0x4F]), declared_bytes=decl,
                             over_declared_here="%04x" % g_decl, over_dump_here="%04x" % g,
                             over_dump_on_gb="%04x" % st.end["gsum"]),
        bank0=["complete (MBC5 window)", "pending?", "MISSING 0x0000-0x0BFF (non-MBC5 without --unmap)",
               "complete (read after unmap)"][st.end["bank0"]],
        ext_fingerprints=st.ext, flash_id=st.ids, probe_end=st.end, plan=pl,
        ram_ok=ram_ok, ram_kib=ram_kib, ram_unstable_kib=ram_bad,
        ram_map="".join("x" if k in st.ram_unstable else "." for k in range(ram_kib)),
        ram_note=None if ram_ok or not st.ram else
            "save RAM NOT backed up: %d of %d KiB never read the same twice (floating); the stable %d KiB are in %s.partial.sav (unstable KiB written as FF)"
            % (len(ram_bad), ram_kib, ram_kib - len(ram_bad), stem),
        ram_diag=st.ramdiag,
    )
    rep["rom_ok"] = (st.end["result"] == 0 and st.bad_sums == 0 and st.unstable == len(st.ram_unstable_chunks) and got_rom == want_rom and
                 not holes_missing and rep["header_checksum"]["header"] == hsum and
                 rep["global_checksum"]["header"] == rep["global_checksum"]["over_declared_here"] and
                 rep["global_checksum"]["over_dump_here"] == rep["global_checksum"]["over_dump_on_gb"])
    rep["ok"] = rep["rom_ok"] and (ram_ok or not st.ram)
    if st.ext:
        decl = pl["rom_banks"]
        uniq = {}
        for e in st.ext:
            uniq.setdefault((e["sum"], e["head"]), []).append(e["bank"])
        blank = (0x4000 * 0xFF) & 0xFFFF
        rep["ext_summary"] = dict(
            banks=len(st.ext),
            all_ff=[e["bank"] for e in st.ext if e["sum"] == blank and e["head"] == "ff" * 16][:8],
            mirrors_of_declared=sum(1 for e in st.ext if e["head"] == rom[(e["bank"] % decl) * 0x4000:(e["bank"] % decl) * 0x4000 + 16].hex()),
            distinct_non_ff=[e["bank"] for e in st.ext if not (e["sum"] == blank and e["head"] == "ff" * 16)][:32])
    json.dump(rep, open(os.path.join(a.out, "report.json"), "w"), indent=1)
    st.log(ev="end", ok=rep["ok"], rom_ok=rep["rom_ok"], ram_ok=ram_ok, ram_map=rep["ram_map"], rom=rep["rom_bytes"], crc32=rep["rom_crc32"], sha256=rep["rom_sha256"],
           global_checksum=rep["global_checksum"], header_checksum=rep["header_checksum"], bank0=rep["bank0"],
           rereads=st.rereads, unstable=st.unstable, report=os.path.join(a.out, "report.json"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--port-file")
    ap.add_argument("--out", default=os.path.expanduser("~/ChromaticCartBackups/" + datetime.datetime.now().strftime("%Y%m%d_%H%M%S")))
    ap.add_argument("--probe", default=os.path.join(HERE, "..", "sm83", "build", "cartprobe.bin"))
    ap.add_argument("--rom-banks", type=int, default=0, help="dump this many 16 KiB banks (default: header size)")
    ap.add_argument("--ram-banks", type=int)
    ap.add_argument("--ram-kb", type=int, default=8)
    ap.add_argument("--extended", action="store_true", help="fingerprint banks past the declared size")
    ap.add_argument("--ext-banks", type=int, default=512)
    ap.add_argument("--allow-flash-id", action="store_true", help="step 2, OWNER APPROVAL: needs the CP_FLASHID probe")
    ap.add_argument("--unmap", action="store_true", help="non-MBC5: read bank 0 under the boot ROM (no normal boot after)")
    ap.add_argument("--exit-on-end", action="store_true")
    ap.add_argument("--mimic-prod", action="store_true", help="answer 404 to everything (a test of the probe's guard)")
    a = ap.parse_args()
    g = in_git_tree(a.out)
    if g:
        sys.exit("refusing --out %s: it is inside the git work tree %s (a cart dump must never be committed)" % (a.out, g))
    if not os.path.exists(a.probe):
        sys.exit("no probe program %s (run sm83/make_sm83.sh)" % a.probe)
    H.st = State(a)
    srv = ThreadingHTTPServer((a.host, a.port), H)
    port = srv.server_address[1]
    if a.port_file:
        open(a.port_file, "w").write(str(port))
    H.st.log(ev="listening", host=a.host, port=port, out=a.out, probe=a.probe,
             probe_sha256=hashlib.sha256(open(a.probe, "rb").read()).hexdigest())
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        while not H.st.done.wait(0.5):
            pass
        if not a.exit_on_end:
            while True:
                H.st.done.wait(3600)
    except KeyboardInterrupt:
        pass
    srv.shutdown()


if __name__ == "__main__":
    main()
