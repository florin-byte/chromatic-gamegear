#!/usr/bin/env python3
"""e2e_local.py: two "Chromatics" (shl_sameboy: SameBoy + window model + the
real ESP32 SHLINK code) through a LOCAL shiny_multiplayer, running a SHLINK
v1.1 cartridge (the link test cartridge by default). One hosts from its room
menu, the other joins by the code its OSD would show.

    e2e_local.py --relay-bin PATH/shiny_multiplayer ROM [--frames N] [--mode code|mine]

Prints one JSON line: messages each way, errors, ms per message, bytes/s.
LOCAL ONLY: tickets are minted with the local relay's own throwaway key.
"""
import base64, hashlib, hmac, json, os, socket, subprocess, sys, tempfile, time, uuid

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port(kind=socket.SOCK_STREAM):
    reserved = {3456} | set(range(5001, 5016)) | {5050} | set(range(6001, 6016)) | {6050}
    while True:
        s = socket.socket(socket.AF_INET, kind)
        s.bind(("127.0.0.1", 0))
        p = s.getsockname()[1]
        s.close()
        if p not in reserved:
            return p


def mint(key, sub, dev):
    now = int(time.time())
    claims = {"sub": sub, "name": "chromatic", "aud": "shinygen-mp", "iat": now, "exp": now + 120,
              "jti": str(uuid.uuid4()), "type": "mp_ticket", "dev": dev}
    b64 = lambda d: base64.urlsafe_b64encode(d).rstrip(b"=")
    msg = b64(json.dumps({"alg": "HS256", "typ": "JWT"}).encode()) + b"." + b64(json.dumps(claims).encode())
    return (msg + b"." + b64(hmac.new(key.encode(), msg, hashlib.sha256).digest())).decode()


class Dev:
    def __init__(self, relay_udp, name):
        self.port = free_port()
        self.name = name
        self.p = subprocess.Popen([os.path.join(HERE, "shl_sameboy"), "--relay", f"127.0.0.1:{relay_udp}",
                                   "--control", str(self.port)], stderr=open(f"/tmp/shl_{name}.log", "w"))
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", self.port))
                break
            except OSError:
                time.sleep(0.05)
        self.f = self.s.makefile("rwb")

    def cmd(self, line):
        self.f.write((line + "\n").encode())
        self.f.flush()
        r = self.f.readline().decode().strip()
        if not r.startswith("ok"):
            raise RuntimeError(f"{self.name}: {line[:40]} -> {r}")
        return r[3:]

    def status(self):
        return json.loads(self.cmd("status"))

    def read(self, a, n):
        return bytes.fromhex(self.cmd(f"read {a} {n}"))

    def lkstat(self):
        if not getattr(self, "lk_at", None):
            ram = self.read(0xC000, 0x2000)
            at = ram.find(b"LKSTAT")
            if at < 0:
                return None
            self.lk_at = 0xC000 + at
        b = self.read(self.lk_at, 24)
        u = lambda o: b[o] | b[o + 1] << 8
        return {"tx": u(6), "rx": u(8), "bad": u(10), "echo": u(12), "full": u(14), "sessions": b[16], "flags": b[17]}

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        self.p.wait(timeout=5)


def main():
    a = sys.argv[1:]
    relay_bin = a[a.index("--relay-bin") + 1]
    frames = int(a[a.index("--frames") + 1]) if "--frames" in a else 6000
    mode = a[a.index("--mode") + 1] if "--mode" in a else "code"
    rom = [x for x in a if x.endswith(".gbc")][0]
    tmp = tempfile.mkdtemp()
    key = uuid.uuid4().hex
    open(os.path.join(tmp, "k"), "w").write(key)
    tcp, udp = free_port(), free_port(socket.SOCK_DGRAM)
    env = dict(os.environ, SHINY_MP_JWT_KEY_FILE=os.path.join(tmp, "k"), SHINY_MULTIPLAYER_PORT=str(tcp),
               SHINY_LINK_UDP_BIND=f"127.0.0.1:{udp}", RUST_LOG="info")
    relay = subprocess.Popen([relay_bin], env=env, stdout=open("/tmp/shl_relay.log", "w"), stderr=subprocess.STDOUT)
    time.sleep(0.8)
    A, B = Dev(udp, "A"), Dev(udp, "B")
    out = {}
    try:
        boot_frames = {}
        for d in (A, B):
            d.cmd(f"load {rom}")
            n = 0
            while not d.status()["attached"] and n < 3000:
                d.cmd("step 10")
                n += 10
            boot_frames[d.name] = n
        acct_b = "acct-A" if mode == "mine" else "acct-B"
        A.cmd("room " + ("mine" if mode == "mine" else "host"))
        A.cmd("ticket " + mint(key, "acct-A", "a000000000000001"))
        t0 = time.time()
        while time.time() - t0 < 10:
            A.cmd("step 2")
            st = A.status()
            if st["in_room"]:
                break
        code = A.status()["code"]
        B.cmd("room " + ("mine" if mode == "mine" else f"join {code}"))
        B.cmd("ticket " + mint(key, acct_b, "b000000000000002"))
        t_join = time.time()
        while time.time() - t_join < 10:
            for d in (A, B):
                d.cmd("step 2")
            if B.status()["in_room"] and A.status()["peers"] == 1:
                break
        join_ms = (time.time() - t_join) * 1000
        # both play in real time (60 fps), lockstep in small slices
        t1 = time.time()
        f = 0
        lags = []
        while f < frames:
            for d in (A, B):
                d.cmd("step 2")
            f += 2
            lag = t1 + f / 60.0 - time.time()
            if lag > 0:
                time.sleep(lag)
            sa, sb = A.lkstat(), B.lkstat()
            # one message a frame each way: how many frames behind the receiver is = latency
            if sa and sb and 0 < sa["tx"] < 600:
                lags.append(max(0, sa["tx"] - sb["rx"]))
                lags.append(max(0, sb["tx"] - sa["rx"]))
            if sa and sb and sa["rx"] >= 600 and sb["rx"] >= 600:
                break
        secs = time.time() - t1
        sa, sb = A.lkstat(), B.lkstat()
        out = {"boot_frames": boot_frames, "mode": mode, "code": code, "join_ms": round(join_ms), "secs": round(secs, 2), "frames": f,
               "A": sa, "B": sb, "A_dev": A.status(), "B_dev": B.status(),
               "msgs_per_s": round((sa["rx"] + sb["rx"]) / max(secs, 1e-3), 1),
               "lag_frames_p50": sorted(lags)[len(lags) // 2] if lags else None,
               "lag_frames_p95": sorted(lags)[int(len(lags) * 0.95)] if lags else None,
               "lag_frames_max": max(lags) if lags else None, "lag_samples": len(lags)}
    finally:
        for d in (A, B):
            d.close()
        relay.terminate()
    print(json.dumps(out))
    ok = out.get("A") and out["A"]["rx"] >= 600 and out["B"]["rx"] >= 600 and out["A"]["bad"] == 0 and out["B"]["bad"] == 0
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
