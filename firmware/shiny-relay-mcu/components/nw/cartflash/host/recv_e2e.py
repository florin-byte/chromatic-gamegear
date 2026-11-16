#!/usr/bin/env python3
"""recv_e2e.py (wifirecv1): receive mode + code against a REAL local shiny_server_rs, the unit's side
spoken exactly as cf_install.c speaks it, the sender's side as the site does it, and every answer the
unit takes run through the device's own parser and CTR (cf_wire_check, built from cf_install.c).

usage: recv_e2e.py --api http://127.0.0.1:PORT --rom GAME.gbc   (the local test account from RECV_E2E_EMAIL / RECV_E2E_PASSWORD)
A local backend only (never production). Prints one line per check; exit 0 = all passed.
No token, key or code is printed.
"""
import argparse, base64, json, os, subprocess, sys, tempfile, urllib.request, urllib.error

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True)
ap.add_argument("--rom", required=True)
ap.add_argument("--email", default=os.environ.get("RECV_E2E_EMAIL", ""))
ap.add_argument("--password", default=os.environ.get("RECV_E2E_PASSWORD", ""))
a = ap.parse_args()
if not a.api.startswith(("http://127.0.0.1", "http://localhost")):
    sys.exit("a local backend only")
HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "cf_wire_check")
T = tempfile.mkdtemp(prefix="recv_e2e.")
ok_n = bad_n = 0


def check(c, what):
    global ok_n, bad_n
    if c:
        ok_n += 1
        print("  ok   " + what)
    else:
        bad_n += 1
        print("  FAIL " + what)


def call(method, path, body=None, token=None, url=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url or (a.api + path), data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            raw = r.read()
            return r.status, raw
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def j(raw):
    try:
        return json.loads(raw or b"{}")
    except ValueError:
        return {}


rom = open(a.rom, "rb").read()
print("register (no pairing, no account)")
st, raw = call("POST", "/link_device/register", {})
reg = j(raw)
check(st == 200 and reg.get("device_token") and reg.get("device_id"), "POST /link_device/register -> a device token")
dev = reg.get("device_token", "")
st, raw = call("POST", "/link_device/recv/start", {})
check(st == 401, "recv/start without a credential: 401 (%d)" % st)

print("RECEIVE GAME: a code")
st, raw = call("POST", "/link_device/recv/start", {}, dev)
open(os.path.join(T, "start.json"), "wb").write(raw)
s = j(raw)
code = s.get("code", "")
check(st == 200 and subprocess.run([CHECK, "start", os.path.join(T, "start.json")], capture_output=True).returncode == 0,
      "recv/start -> a code the device takes (cf_wire_check start)")
check(s.get("expires_in") == 600 and 2000 <= s.get("poll_ms", 0) <= 10000, "valid 10 minutes, poll_ms %s" % s.get("poll_ms"))
st, raw = call("POST", "/link_device/recv/poll", {"code": code}, dev)
check(st == 200 and j(raw).get("session") == "waiting", "recv/poll: waiting")

print("the sender (signed in, not paired)")
st, raw = call("POST", "/login", {"email_or_username": a.email, "password": a.password})
acc = j(raw).get("access_token", "")
check(st == 200 and acc, "the sender signs in")
b64 = base64.b64encode(rom).decode()
st, raw = call("POST", "/cart_install", {"rom": b64, "code": code})
check(st == 401, "send while signed out: 401 (%d)" % st)
wrong = ("B" if code[0] != "B" else "C") + code[1:]
st, raw = call("POST", "/cart_install/check", {"code": wrong}, acc)
check(st == 404, "a wrong code: 404 (%d)" % st)
st, raw = call("POST", "/cart_install/check", {"code": "AB"}, acc)
check(st == 400, "a malformed code: 400 (%d)" % st)
st, raw = call("POST", "/cart_install/check", {"code": code.lower()}, acc)
check(st == 200, "check the code (lower case forgiven): 200 (%d)" % st)
st, raw = call("POST", "/cart_install", {"rom": b64, "code": code}, acc)
check(st == 200 and j(raw).get("install", {}).get("state") == "pending", "send by code: 200, pending")
st, raw = call("POST", "/cart_install", {"rom": b64, "code": code}, acc)
check(st == 410, "the same code again: 410 (single use) (%d)" % st)

print("the unit's next poll: the game")
st, raw = call("POST", "/link_device/recv/poll", {"code": code}, dev)
open(os.path.join(T, "poll.json"), "wb").write(raw)
p = j(raw)
check(st == 200 and p.get("session") == "sent", "recv/poll: sent")
blob = p.get("blob_url", "")
# the device downloads the ciphertext over plain http with no Authorization header, 2,048 bytes a range
ct = bytearray()
okr = True
while len(ct) < len(rom):
    req = urllib.request.Request(blob)
    req.add_header("Range", "bytes=%d-%d" % (len(ct), min(len(rom), len(ct) + 2048) - 1))
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            ct += r.read()
    except urllib.error.HTTPError as e:
        okr = False
        print("    blob range %d: %d" % (len(ct), e.code))
        break
check(okr and len(ct) == len(rom), "the blob, by ranges, no Authorization header (%d bytes)" % len(ct))
check(bytes(ct) != rom, "the blob is ciphertext")
open(os.path.join(T, "blob.bin"), "wb").write(ct)
r = subprocess.run([CHECK, "offer", os.path.join(T, "poll.json"), os.path.join(T, "blob.bin"), os.path.join(T, "plain.bin")], capture_output=True, text=True)
for line in r.stdout.splitlines():
    print("    " + line)
check(r.returncode == 0 and open(os.path.join(T, "plain.bin"), "rb").read() == rom,
      "the device's parser takes the offer and its CTR gives back the ROM byte for byte")
iid = p.get("id", "")
st, raw = call("POST", "/link_device/recv/resume", {"id": iid}, dev)
check(st == 200 and j(raw).get("id") == iid and j(raw).get("enc_key"), "recv/resume {id}: the same offer (an unfinished install)")
st, raw = call("POST", "/link_device/register", {})
dev2 = j(raw).get("device_token", "")
st, raw = call("POST", "/link_device/recv/resume", {"id": iid}, dev2)
check(st == 200 and j(raw).get("pending") is False, "another unit cannot resume it")
st, raw = call("POST", "/link_device/cart_install/ack", {"id": iid, "state": "installed"}, dev2)
check(st == 404, "another unit cannot report on it (%d)" % st)
st, raw = call("POST", "/link_device/cart_install/ack", {"id": iid, "state": "downloading", "detail": ""}, dev)
check(st == 200, "'downloading' from the unit")
st, raw = call("GET", "/cart_install", None, acc)
check(j(raw).get("install", {}).get("state") == "downloading", "the sender sees downloading")
st, raw = call("POST", "/link_device/cart_install/ack", {"id": iid, "state": "installed", "detail": ""}, dev)
check(st == 200, "'installed' from the unit")
st, raw = call("GET", "/cart_install", None, acc)
check(j(raw).get("install", {}).get("state") == "installed", "the sender sees installed")
req = urllib.request.Request(blob)
req.add_header("Range", "bytes=0-15")
try:
    urllib.request.urlopen(req, timeout=30)
    gone = False
except urllib.error.HTTPError as e:
    gone = e.code == 404
check(gone, "the blob is gone once installed")
st, raw = call("POST", "/link_device/recv/poll", {"code": code}, dev)
check(j(raw).get("session") == "expired", "the code is over after the install")

print("B on the unit")
st, raw = call("POST", "/link_device/recv/start", {}, dev)
c2 = j(raw).get("code", "")
st, raw = call("POST", "/link_device/recv/cancel", {"code": c2}, dev)
check(st == 200 and j(raw).get("session") == "closed", "recv/cancel: closed")
st, raw = call("POST", "/cart_install/check", {"code": c2}, acc)
check(st == 410, "a cancelled code: 410 for the sender (%d)" % st)
st, raw = call("POST", "/link_device/recv/poll", {"code": c2}, dev)
check(j(raw).get("session") == "expired", "and expired for the unit")
st, raw = call("POST", "/link_device/recv/start", {}, dev)
c3 = j(raw).get("code", "")
st, raw = call("POST", "/link_device/recv/start", {}, dev)
c4 = j(raw).get("code", "")
st, raw = call("POST", "/link_device/recv/poll", {"code": c3}, dev)
check(c3 != c4 and j(raw).get("session") == "expired", "a new code closes the unit's previous one")
st, raw = call("POST", "/link_device/recv/poll", {"code": c4}, dev2)
check(j(raw).get("session") == "expired", "another unit cannot poll a unit's code")

print("guesses")
codes = []
for i in range(12):
    g = "".join("BCDFGHJKMNPQRSTVWXZ23456789"[(i * 7 + k * 3) % 27] for k in range(6))
    st, raw = call("POST", "/cart_install/check", {"code": g}, acc)
    codes.append(st)
check(codes.count(429) >= 1, "wrong-code guesses run into 429 (%s)" % codes)

print("recv_e2e: %d ok, %d failed" % (ok_n, bad_n))
sys.exit(1 if bad_n else 0)
