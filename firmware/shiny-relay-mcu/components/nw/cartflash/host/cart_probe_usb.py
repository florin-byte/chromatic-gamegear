#!/usr/bin/env python3
"""cart_probe_usb.py: the read-only cart probe over the Chromatic's USB console (lane cartflash1).

No Wi-Fi, no http, no `nw url` change: it works with RELEASE ESP32 builds that have
CONFIG_NW_CARTFLASH (0.17.1+ refuse plain http, so the LAN receiver needs a debug build).
The flow:

  1. this tool sends `cart probe` (or `cart probe flashid`, OWNER APPROVAL) to the console;
  2. the ESP32 arms the probe and power-cycles the cart: the netboot stub runs with the
     ESP32 already up (no race with an FPGA SRAM load) and gets the probe from CF_LOAD;
  3. the bulk data (1 KiB chunks, checked by the ESP32 and answered to the GB at once) comes
     out of the console as binary frames  A5 'C' 'P' 'B' letter len16 body crc32  while the
     next chunk is read; the few control calls (hello, header/plan, flash ID, diag, end) come
     as lines  CPX <id> <G|P> <path> <base64 body>  and this tool answers each with the
     console command `cart px <id> <nw status> <base64>`,
     using exactly the receiver logic of cart_probe_rx.py (same plan, same files, same
     report.json in --out, never inside a git tree).

A cart dump is a copyrighted commercial game: it goes only to --out on this computer.

Transports (one of):
  --daemon            the running tools/console_daemon.py (its LOG + FIFO), the default;
  --serial PORT       open the port directly (DTR/RTS low: opening must not reset the ESP32);
  --stdio             lines on stdin / commands on stdout (the simulation, host/cf_sim.c).

  python3 cart_probe_usb.py [--daemon | --serial P | --stdio] [--no-arm] [--flash-id]
        [--rom-banks N] [--ram-banks N --ram-kb K] [--extended] [--out DIR] [--timeout S]
"""
import argparse
import base64
import binascii
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cart_probe_rx as rx  # noqa: E402

MAGIC = b"\xa5CPB"
STATUS = {200: 0, 206: 0, 400: 1, 401: 3, 403: 3, 404: 4, 413: 8, 429: 9}


class Stdio:
    def __init__(self):
        self.buf = b""

    def send(self, cmd):
        sys.stdout.write(cmd + "\n")
        sys.stdout.flush()

    def read(self):
        d = os.read(sys.stdin.fileno(), 65536)
        if not d:
            raise EOFError
        return d


class Daemon:
    def __init__(self, log, fifo):
        self.log, self.fifo = log, fifo
        self.f = open(log, "rb")
        self.f.seek(0, 2)
        self.buf = b""

    def send(self, cmd):
        with open(self.fifo, "w") as w:
            w.write(cmd + "\n")

    def read(self):
        d = self.f.read()
        if not d:
            time.sleep(0.005)
        return d or b""


class Serial:
    def __init__(self, port):
        import serial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.05
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.buf = b""

    def send(self, cmd):
        self.s.write(b"\r\n")          # wake a light-sleeping MCU (console_daemon.py does the same)
        time.sleep(0.15)
        self.s.write(cmd.encode() + b"\r\n")

    def read(self):
        return self.s.read(65536) or b""


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--stdio", action="store_true")
    g.add_argument("--serial")
    g.add_argument("--daemon", action="store_true")
    ap.add_argument("--daemon-log", default="${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/console.log")
    ap.add_argument("--daemon-fifo", default="${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/console.fifo")
    ap.add_argument("--no-arm", action="store_true", help="do not send `cart probe` (already armed)")
    ap.add_argument("--flash-id", action="store_true", help="step 2, OWNER APPROVAL: the flash-ID probe")
    ap.add_argument("--out", default=os.path.expanduser("~/ChromaticCartBackups/" + time.strftime("%Y%m%d_%H%M%S")))
    ap.add_argument("--rom-banks", type=int, default=0)
    ap.add_argument("--ram-banks", type=int)
    ap.add_argument("--ram-kb", type=int, default=8)
    ap.add_argument("--extended", action="store_true")
    ap.add_argument("--ext-banks", type=int, default=512)
    ap.add_argument("--unmap", action="store_true")
    ap.add_argument("--timeout", type=float, default=1800)
    a = ap.parse_args()
    gt = rx.in_git_tree(a.out)
    if gt:
        sys.exit("refusing --out %s: inside the git work tree %s" % (a.out, gt))
    # the receiver's options, as cart_probe_rx.py has them
    a.allow_flash_id = a.flash_id
    a.mimic_prod = False
    a.probe = None                     # the probe program comes from the ESP32 (CF_LOAD) in this mode
    t = Stdio() if a.stdio else Serial(a.serial) if a.serial else Daemon(a.daemon_log, a.daemon_fifo)
    st = rx.State(a)
    if a.stdio:                        # stdout carries console commands: the log goes to stderr
        st.log = lambda **kw: print(__import__("json").dumps(kw), file=sys.stderr, flush=True)
    if not a.no_arm:
        t.send("cart probe flashid" if a.flash_id else "cart probe")
    st.log(ev="armed" if not a.no_arm else "listening", transport="stdio" if a.stdio else a.serial or "daemon", out=a.out)
    deadline = time.time() + a.timeout
    buf = b""
    nframes = nbad = 0
    while not st.done.is_set():
        if time.time() > deadline:
            st.log(ev="timeout")
            return 1
        try:
            buf += t.read()
        except EOFError:
            st.log(ev="eof")
            return 1
        while True:
            fi, li = buf.find(MAGIC), buf.find(b"CPX ")
            if fi < 0 and li < 0:
                buf = buf[-3:]                      # keep a possibly split magic / "CPX"
                break
            if fi >= 0 and (li < 0 or fi < li):    # binary frame: A5 'CPB' letter len body crc
                if len(buf) < fi + 7:
                    buf = buf[fi:]
                    break
                n = buf[fi + 5] | buf[fi + 6] << 8
                if len(buf) < fi + 11 + n:
                    buf = buf[fi:]
                    break
                fr = buf[fi + 4:fi + 7 + n]
                crc = int.from_bytes(buf[fi + 7 + n:fi + 11 + n], "little")
                if binascii.crc32(fr) & 0xFFFFFFFF != crc or n > 2048:
                    nbad += 1                       # not a frame (or damaged): resync past the magic
                    st.log(ev="bad_frame", at=fi, n=n)
                    buf = buf[fi + 1:]
                    continue
                buf = buf[fi + 11 + n:]
                nframes += 1
                rx.handle(st, "POST", "cp/" + chr(fr[0]), fr[3:], "usb")
                continue
            nl = buf.find(b"\n", li)
            if nl < 0:
                buf = buf[li:]
                break
            parts = buf[li:nl].decode("latin-1").rstrip("\r").split(" ")
            buf = buf[nl + 1:]
            if len(parts) < 4:
                continue
            cid, meth, path = parts[1], parts[2], parts[3]
            body = base64.b64decode(parts[4]) if len(parts) > 4 and parts[4] else b""
            code, out = rx.handle(st, "GET" if meth == "G" else "POST", path, body, "usb")
            t.send("cart px %s %u %s" % (cid, STATUS.get(code, 7), base64.b64encode(out).decode()))
    st.log(ev="stream", frames=nframes, bad_frames=nbad)
    st.log(ev="finished", report=os.path.join(a.out, "report.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
