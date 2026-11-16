#!/usr/bin/env python3
"""cart_usb.py: reference implementation of the Chromatic's USB cart backup / restore
protocol CBU1 (docs/CART_BACKUP_USB_PROTOCOL.md; device side components/nw/cartflash/cf_usb.c).
A browser page ports this file: the transport is one serial port (WebSerial), 115200 8N1.

  cart_usb.py --port /dev/cu.usbmodemXXXX info
  cart_usb.py --port ... backup  --rom out.gbc --sav out.sav
  cart_usb.py --port ... restore --rom my.gbc [--sav my.sav]
  cart_usb.py --port ... status | cancel
  cart_usb.py --stdio ...        the simulated device (host/cf_sim --usb-tool): device bytes on
                                 stdin, commands on stdout

Everything stays on this computer: nothing is uploaded, nothing is stored on the Chromatic.
Exit code 0 = done and verified.
"""
import argparse, base64, binascii, hashlib, json, os, sys, time

MAGIC = b"\xA5CBU"
WRITABLE_DEFAULT = 0x1F0000


_T0 = time.time()


def log(**k):
    k["t"] = round(time.time() - _T0, 1)             # seconds since the tool started: where the time goes
    print(json.dumps(k), file=sys.stderr, flush=True)


class Dev:
    """The byte stream from the device, split into CBU1 text lines and binary frames."""

    def __init__(self, rd, wr):
        self.rd, self.wr, self.buf = rd, wr, b""
        self.bad_frames = 0

    def cmd(self, s):
        self.wr(("cart usb " + s + "\n").encode())

    def events(self, timeout):
        """Yield ('line', str) for every CBU1 line and ('frame', type, offset, data) for every good
        frame; ('timeout',) when nothing arrived for `timeout` seconds."""
        last = time.time()
        while True:
            ev = self._take()
            if ev:
                last = time.time()
                if ev[0] != "skip":      # other console output, a bad frame: look at the buffer again at once
                    yield ev
                continue
            d = self.rd()
            if d:
                self.buf += d
                last = time.time()
            elif time.time() - last > timeout:
                yield ("timeout",)
                last = time.time()

    def _take(self):
        b = self.buf
        i = b.find(MAGIC)
        j = b.find(b"\n")
        if i >= 0 and (j < 0 or i < j):
            # a frame starts before the next line end: MAGIC type off32 len16 data crc32
            if i > 0:
                self.buf = b[i:]
                b = self.buf
            if len(b) < 11:
                return None
            n = b[9] | b[10] << 8
            if n > 1024:
                self.buf = b[1:]
                self.bad_frames += 1
                return ("skip",)
            if len(b) < 15 + n:
                return None
            body, crc = b[4:11 + n], int.from_bytes(b[11 + n:15 + n], "little")
            self.buf = b[15 + n:]
            if binascii.crc32(body) & 0xFFFFFFFF != crc:
                self.bad_frames += 1
                return ("skip",)
            return ("frame", chr(body[0]), int.from_bytes(body[1:5], "little"), bytes(body[7:]))
        if j < 0:
            if len(b) > 65536 and i < 0:
                self.buf = b[-3:]
            return None
        line, self.buf = b[:j], b[j + 1:]
        k = line.find(b"CBU1 ")
        if k < 0:
            return ("skip",)                 # other console output: ignored
        return ("line", line[k:].decode("ascii", "replace").strip())


def wait_ready(dev, timeout=60):
    """After every job the device says `CBU1 ready` when it can take the next command."""
    for ev in dev.events(timeout):
        if ev[0] == "line" and ev[1].startswith("CBU1 ready"):
            return True
        if ev[0] == "timeout":
            return False


def kv(line):
    out = {}
    for tok in line.split():
        if "=" in tok:
            a, b = tok.split("=", 1)
            out[a] = b.strip('"')
    if 'title="' in line:
        out["title"] = line.split('title="', 1)[1].rsplit('"', 1)[0]
    return out


def info(dev, timeout=180):
    dev.cmd("info")
    for ev in dev.events(timeout):
        if ev[0] == "line" and ev[1].startswith("CBU1 info"):
            wait_ready(dev)
            return kv(ev[1])
        if ev[0] == "line" and ev[1].startswith("CBU1 busy"):
            time.sleep(1.0)
            dev.cmd("info")
            continue
        if ev[0] == "line" and ev[1].startswith("CBU1 fail"):
            raise SystemExit("device: " + ev[1])
        if ev[0] == "timeout":
            raise SystemExit("no answer to `cart usb info` (is this firmware built with the cart feature, and the FPGA able to reach the cart?)")


def read_range(dev, what, off, length, out, timeout=120):
    """Read [off, off+length) of 'rom' or 'sav' into bytearray out. Returns the device's crc32 of
    what it sent, or None when the stream stopped (the caller asks again for what is missing)."""
    # skipblank (firmware 0.18.0-rc1; older firmware ignores the word): a 16 KiB bank that reads all
    # FF is not sent, only named ("CBU1 blank <offset> <length>"). Most of a cart is usually empty.
    dev.cmd(f"read {what} {off} {length}" + (" skipblank" if what == "rom" else ""))
    typ = "R" if what == "rom" else "S"
    for ev in dev.events(timeout):
        if ev[0] == "frame" and ev[1] == typ:
            out[ev[2]:ev[2] + len(ev[3])] = ev[3]
            got.update(range(ev[2] // 256, (ev[2] + len(ev[3]) + 255) // 256))
        elif ev[0] == "line" and ev[1].startswith("CBU1 blank "):
            bo, bl = int(ev[1].split()[2]), int(ev[1].split()[3])
            out[bo:bo + bl] = b"\xFF" * bl
            got.update(range(bo // 256, (bo + bl + 255) // 256))
        elif ev[0] == "line" and ev[1].startswith("CBU1 end " + what):
            wait_ready(dev)
            return int(ev[1].split()[5], 16)
        elif ev[0] == "line" and ev[1].startswith("CBU1 busy"):
            time.sleep(0.5)              # a job is still ending: ask again
            return None
        elif ev[0] == "line" and ev[1].startswith("CBU1 fail"):
            log(ev="fail", line=ev[1])
            if "busy" in ev[1]:
                time.sleep(0.2)          # a job is still ending: ask again
            else:
                wait_ready(dev, 10)
            return None
        elif ev[0] == "timeout":
            return None


got = set()


def read_all(dev, what, size):
    """The whole ROM / save, with resume: after a stop, only the missing ranges are asked again."""
    global got
    got = set()
    out = bytearray(b"\xFF" * size)
    tries = 0
    while True:
        missing = [p for p in range((size + 255) // 256) if p not in got]
        if not missing:
            break
        a = missing[0] * 256
        b = a
        while b // 256 in missing and b < size:
            b += 256
        b = min(b, size)
        if a != 0 or b != size:
            # 1 KiB frames: align the request down to a frame boundary
            a = a // 1024 * 1024
        crc = read_range(dev, what, a, b - a, out)
        tries += 1
        if crc is not None and binascii.crc32(out[a:b]) & 0xFFFFFFFF != crc:
            for p in range(a // 256, (b + 255) // 256):
                got.discard(p)           # the range as a whole is wrong: again
        if tries > 40:
            raise SystemExit(f"{what}: could not read everything after {tries} tries")
    log(ev="read", what=what, bytes=size, requests=tries, bad_frames=dev.bad_frames)
    return bytes(out)


def rom_length(rom):
    """What to save: the header's ROM size when the header is valid and everything after it is
    FF; else the last non-FF byte rounded up to 32 KiB (the whole chip never needs saving)."""
    hc = 0
    for x in rom[0x134:0x14D]:
        hc = (hc - x - 1) & 0xFF
    n = 0x8000 << rom[0x148] if rom[0x148] <= 8 else 0
    if hc == rom[0x14D] and 0 < n <= len(rom) and rom[n:].count(0xFF) == len(rom) - n:
        return n
    end = len(rom.rstrip(b"\xFF"))
    return min(len(rom), max(0x8000, (end + 0x7FFF) // 0x8000 * 0x8000))


def backup(dev, a):
    i = info(dev)
    flash, ram = int(i["flash"]), int(i["ram"])
    log(ev="info", **i)
    rom = read_all(dev, "rom", flash)
    sav = read_all(dev, "sav", ram)
    n = rom_length(rom)
    open(a.rom, "wb").write(rom[:n])
    open(a.sav, "wb").write(sav)
    log(ev="backup", title=i.get("title", ""), rom=a.rom, rom_bytes=n, rom_crc32="%08x" % (binascii.crc32(rom[:n]) & 0xFFFFFFFF),
        rom_sha256=hashlib.sha256(rom[:n]).hexdigest(), sav=a.sav, sav_bytes=len(sav), sav_crc32="%08x" % (binascii.crc32(sav) & 0xFFFFFFFF))


MAX_TRIES_PER_CHUNK = 40


def feed(dev, data, done_word, timeout=60):
    """Answer the device's `CBU1 want <offset> <length>` until it says done / fail.

    Strict stop-and-wait: ONE data line per `want`, never anything else in between. The line for
    a 96-byte chunk is ~160 characters and must fit the console's receive buffer whole (it holds
    384 and is emptied at only ~1.3 characters per ms): never send a second line before the
    device has answered the first. The device's answer to a good line is its next `want` (or
    done); to a damaged one, the same `want` again. Returns (ok, last line, lines sent)."""
    sent, tries, last_off = 0, 0, -1
    for ev in dev.events(timeout):
        if ev[0] == "line" and ev[1].startswith("CBU1 want "):
            f = ev[1].split()
            try:
                off, n = int(f[2]), int(f[3])
            except (IndexError, ValueError):
                continue                             # a damaged `want`: the device repeats it
            if n <= 0 or n > 1024 or off + n > len(data):
                return False, f"bad want {off}+{n}", sent
            tries = tries + 1 if off == last_off else 1
            last_off = off
            if tries > MAX_TRIES_PER_CHUNK:
                return False, f"chunk {off} refused {tries - 1} times", sent
            chunk = data[off:off + n]
            if len(f) >= 5 and f[4] == "bin":
                # binary mode: A5 'C' 'B' 'D', offset u32, length u16, data, crc32 over offset..data
                body = off.to_bytes(4, "little") + n.to_bytes(2, "little") + chunk
                dev.wr(b"\xA5CBD" + body + (binascii.crc32(body) & 0xFFFFFFFF).to_bytes(4, "little"))
            else:
                dev.cmd("d %d %s %08x" % (off, base64.b64encode(chunk).decode(), binascii.crc32(chunk) & 0xFFFFFFFF))
            sent += 1
        elif ev[0] == "line" and ev[1].startswith("CBU1 done " + done_word):
            wait_ready(dev)
            return True, ev[1], sent
        elif ev[0] == "line" and ev[1].startswith("CBU1 text"):
            return False, ev[1], sent                # the device gave the console back (no frame for 30 s): send the command again
        elif ev[0] == "line" and ev[1].startswith("CBU1 busy"):
            return False, ev[1], sent                # another job is still ending: the caller waits and asks again
        elif ev[0] == "line" and ev[1].startswith("CBU1 fail"):
            wait_ready(dev, 10)
            return False, ev[1], sent
        elif ev[0] == "timeout":
            return False, "timeout", sent


def write_job(dev, cmd, data, done_word, timeout, attempts=12, binary=True):
    """A write with every recoverable answer handled: busy / usage / memory / timeout / an
    unfinished write are all "send the same command again" (the device carries on from where the
    cart is). Only a refusal that cannot change stops it."""
    ok, line, sent = False, "", 0
    for attempt in range(attempts):
        # "bin": the device takes binary frames (5 KiB/s and more). After two attempts that did
        # not finish, the slow text lines (under 1 KiB/s) are used: they need nothing but a console.
        dev.cmd(cmd + (" bin" if binary and attempt < 2 else ""))
        ok, line, sent = feed(dev, data, done_word, timeout=timeout)
        log(ev="write_" + done_word, ok=ok, line=line, lines=sent, attempt=attempt + 1)
        if ok:
            return True, line
        if "code=12" in line or "code=13" in line or "code=14" in line or "code=7" in line or "code=2 " in line or "refused 6" in line or line.startswith("bad want"):
            return False, line                       # a patch is active / write-protected / does not fit / not a flash cart / another file is unfinished
        time.sleep(2.0 if "busy" in line or "memory" in line else 0.5)
        for ev in dev.events(0.3):                   # let what is on its way pass
            if ev[0] == "timeout":
                break
    return False, line


def accept_rule(rom, writable=WRITABLE_DEFAULT):
    """The size rule (the backend has the same one): 32,768 .. 2,097,152 bytes, and every byte at
    offset >= the writable size (2,031,616) must be FF."""
    return 32768 <= len(rom) <= 2097152 and rom[writable:].count(0xFF) == max(0, len(rom) - writable)


def restore(dev, a):
    rom = open(a.rom, "rb").read()
    if not accept_rule(rom):
        raise SystemExit("this file cannot go on the cart: 32,768 to 2,031,616 bytes (or up to 2,097,152 when the rest is FF)")
    crc, sha = binascii.crc32(rom) & 0xFFFFFFFF, hashlib.sha256(rom).hexdigest()
    ok, line = write_job(dev, "write rom %d %08x %s" % (len(rom), crc, sha), rom, "rom", a.timeout, binary=not a.text)
    if not ok:
        raise SystemExit("ROM not written: " + line)
    if a.sav:
        sav = open(a.sav, "rb").read()
        if len(sav) != 32768:
            raise SystemExit("the save file must be exactly 32,768 bytes")
        ok, line = write_job(dev, "write sav", sav, "sav", a.timeout, binary=not a.text)
        if not ok:
            raise SystemExit("save not written: " + line)
        if int(line.split()[3], 16) != binascii.crc32(sav) & 0xFFFFFFFF:
            raise SystemExit("save crc differs")


STAGE_SAV_OFF, STAGE_ROM_OFF, STAGE_ROM_MAX = 0x208000, 0x210000, 0x1F0000


def expect(dev, starts, timeout=60):
    """The next CBU1 line that begins with one of `starts`, or with fail / busy. None on silence."""
    for ev in dev.events(timeout):
        if ev[0] == "line" and (ev[1].startswith(tuple(starts)) or ev[1].startswith(("CBU1 fail", "CBU1 busy"))):
            return ev[1]
        if ev[0] == "timeout":
            return None


def staged_install(dev, a):
    """usb-cart-1: the ROM (and the save) are ALREADY in the ESP32's flash (written in bootloader
    mode: `esptool write_flash 0x210000 rom 0x208000 sav`, which main() does for a real port).
    Here: have the firmware check them, write the cart from them, and see them erased."""
    rom = open(a.rom, "rb").read()
    if not 32768 <= len(rom) <= STAGE_ROM_MAX:
        raise SystemExit("the file must be 32,768 to 2,031,616 bytes (cut the FF bytes off a 2 MiB file)")
    sav = open(a.sav, "rb").read() if a.sav else None
    if sav is not None and len(sav) != 32768:
        raise SystemExit("the save file must be exactly 32,768 bytes")
    crc, sha = binascii.crc32(rom) & 0xFFFFFFFF, hashlib.sha256(rom).hexdigest()
    for attempt in range(8):
        dev.cmd("stage rom %d %08x %s" % (len(rom), crc, sha))
        line = expect(dev, ["CBU1 staged rom"], 120)
        log(ev="stage_rom", line=line, attempt=attempt + 1)
        if line is None or line.startswith("CBU1 busy"):
            time.sleep(1.0)
            continue
        if not line.startswith("CBU1 staged rom"):
            raise SystemExit("ROM not staged: " + line)
        if sav is not None:
            dev.cmd("stage sav %08x" % (binascii.crc32(sav) & 0xFFFFFFFF))
            line = expect(dev, ["CBU1 staged sav"], 30)
            log(ev="stage_sav", line=line)
            if line is None or not line.startswith("CBU1 staged sav"):
                raise SystemExit("save not staged: " + str(line))
        dev.cmd("install" + (" fast" if a.fast else ""))
        last_pct, done, cleared = -1, None, None
        for ev in dev.events(a.timeout):
            if ev[0] == "timeout":
                break
            if ev[0] != "line":
                continue
            t = ev[1]
            if t.startswith("CBU1 progress rom "):
                f = t.split()
                if len(f) >= 7 and f[6].isdigit() and int(f[6]) // 10 != last_pct // 10:
                    last_pct = int(f[6]); log(ev="progress", phase=f[3], percent=last_pct)
            elif t.startswith("CBU1 time ") or t.startswith("CBU1 note "):
                log(ev="device", line=t)             # per-phase times (0.18.0-usb3), words for the user
            elif t.startswith("CBU1 cleared") or t.startswith("CBU1 fail clear"):
                cleared = t
            elif t.startswith("CBU1 done rom") or t.startswith("CBU1 fail"):
                done = t
            elif t.startswith("CBU1 ready"):
                break
        log(ev="install", line=done, cleared=cleared, attempt=attempt + 1)
        if done and done.startswith("CBU1 done rom"):
            if cleared is None or not cleared.startswith("CBU1 cleared"):
                # the game is on the cart, but the device never said its copy is gone
                raise SystemExit("INSTALLED, but the copy of the ROM / save is still in the Chromatic's flash (%s): run `clear`"
                                 % (cleared or "no `CBU1 cleared` from the device"))
            return
        if done and ("code=12" in done or "code=13" in done or "code=14" in done or "code=7" in done or "code=2 " in done or "code=238" in done):
            raise SystemExit("not installed: " + done)
        # code=36 (the device restarted between `stage` and `install`): stage and install again
        time.sleep(1.0)                              # unfinished / cut off: the same commands again carry on
    raise SystemExit("not installed after 8 attempts")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--stdio", action="store_true")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--wait", type=float, default=30, help="seconds to wait for the console after opening the port (the ESP32 restarts: about 12 s)")
    sub = ap.add_subparsers(dest="what", required=True)
    sub.add_parser("info"); sub.add_parser("status"); sub.add_parser("cancel")
    b = sub.add_parser("backup"); b.add_argument("--rom", required=True); b.add_argument("--sav", required=True)
    i2 = sub.add_parser("install", help="usb-cart: stage in the ESP32's flash with esptool, then write the cart from there")
    i2.add_argument("--rom", required=True); i2.add_argument("--sav"); i2.add_argument("--fast", action="store_true", help="per-bank CRC verify")
    i2.add_argument("--no-flash", action="store_true", help="the files are staged already: only the console part")
    sub.add_parser("clear")
    so = sub.add_parser("savonly", help="only the save: write it, the game stays"); so.add_argument("--sav", required=True)
    so.add_argument("--text", action="store_true")
    r = sub.add_parser("restore"); r.add_argument("--rom", required=True); r.add_argument("--sav"); r.add_argument("--text", action="store_true", help="text lines only (slow; for a firmware without binary mode)")
    a = ap.parse_args()
    if a.stdio:
        fd = sys.stdin.fileno()
        os.set_blocking(fd, False)

        def rd():
            try:
                d = os.read(fd, 65536)
            except BlockingIOError:
                d = b""
            if not d:
                time.sleep(0.002)
            return d

        def wr(x):
            sys.stdout.buffer.write(x); sys.stdout.buffer.flush()
    else:
        if a.what == "install" and not a.no_flash:
            # bootloader mode, as the firmware updater does it: the ROM and the save go into the
            # ESP32's own flash, outside the firmware. Nothing on the cart is touched by this.
            import subprocess
            cmd = [sys.executable, "-m", "esptool", "--chip", "esp32", "-p", a.port, "-b", "460800", "write_flash",
                   "--flash_mode", "dio", "--flash_size", "4MB", "--flash_freq", "40m", "0x%x" % STAGE_ROM_OFF, a.rom]
            if a.sav:
                cmd += ["0x%x" % STAGE_SAV_OFF, a.sav]
            t0 = time.time()
            if subprocess.call(cmd) != 0:
                raise SystemExit("esptool could not write the files into the Chromatic's flash")
            log(ev="flashed", seconds=round(time.time() - t0, 1))
        import serial                                # pyserial
        # DTR and RTS OFF BEFORE the port is opened: they are wired to the ESP32's reset / boot
        # pins. Opened with pyserial's defaults (both asserted) the console answers nothing
        # (unit 1, 2026-10-11).
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = a.port, 115200, 0.05
        s.dtr = False
        s.rts = False
        s.open()
        rd, wr = (lambda: s.read(65536)), (lambda x: (s.write(x), s.flush()))
    dev = Dev(rd, wr)
    if not a.stdio:
        # Opening the port restarts the ESP32 all the same (the lines move for a moment as the
        # port opens; nothing in the firmware can prevent that): the console is back after about
        # 12 s. Ask `status` once a second until a CBU1 line comes. A cart write that was
        # running is not lost by the restart: it is "unfinished" and resumes.
        t0 = time.time()
        up = False
        hits = 0
        while not up and time.time() - t0 < a.wait:
            dev.cmd("status")
            got = False
            for ev in dev.events(2.0):
                if ev[0] == "line" and ev[1].startswith("CBU1 status"):
                    got = True                       # only the `status` command prints this
                    break
                if ev[0] == "timeout":
                    break
            # One answer is not enough: the ESP32 may answer once and THEN restart (the reset
            # from opening the port lands late), or the answer is one left from before. Up =
            # two asks at least a second apart, both answered, no silence between.
            hits = hits + 1 if got else 0
            up = hits >= 2
            if got and not up:
                for ev in dev.events(1.2):
                    if ev[0] == "timeout":
                        break
        if not up:
            raise SystemExit("no answer from the Chromatic's console in %d s (cart firmware installed? port right?)" % a.wait)
        log(ev="console", after_s=round(time.time() - t0, 1))
        # the asks sent while it was starting are answered one after the other now: wait until
        # the console has been quiet for a second, so none of those answers lands in a job
        for ev in dev.events(1.0):
            if ev[0] == "timeout":
                break
    if a.what == "info":
        print(json.dumps(info(dev)))
    elif a.what in ("status", "cancel"):
        dev.cmd(a.what)
        for ev in dev.events(10):
            if ev[0] == "line" and ev[1].startswith("CBU1 status") or ev[0] == "timeout":
                print(ev[1] if ev[0] == "line" else "timeout")
                break
    elif a.what == "install":
        staged_install(dev, a)
    elif a.what == "clear":
        dev.cmd("clear")
        line = expect(dev, ["CBU1 cleared", "CBU1 fail clear"], 90)
        print(line)
        if line is None or not line.startswith("CBU1 cleared"):
            raise SystemExit("NOT cleared: the copy of the ROM / save may still be in the Chromatic's flash")
    elif a.what == "backup":
        backup(dev, a)
    elif a.what == "savonly":
        sav = open(a.sav, "rb").read()
        if len(sav) != 32768:
            raise SystemExit("the save file must be exactly 32,768 bytes")
        ok, line = write_job(dev, "write sav", sav, "sav", a.timeout, binary=not a.text)
        if not ok:
            raise SystemExit("save not written: " + line)
        if int(line.split()[3], 16) != binascii.crc32(sav) & 0xFFFFFFFF:
            raise SystemExit("save crc differs")
    else:
        restore(dev, a)
    log(ev="done")


if __name__ == "__main__":
    main()
