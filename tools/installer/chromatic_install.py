#!/usr/bin/env python3
"""Install and maintain the Chromatic Game Boy + Game Gear setup."""
import argparse
import datetime
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import chromatic_flash_image as flash
import gg2gb
from console import Console

FLASH = 0x400000
APP, APP_SIZE, OTA, OTA_SIZE = 0x10000, 0x1E0000, 0x200000, 0x2000
PART = 0x100000
BACKUPS = ("esp32.bin", "fpga.bin")
SHIPPED = ("mcu_fw.bin", "ota_data_initial.bin", "partition-table.bin", "fpga-gb.bin", "fpga-gg.bin")
POWER = "Switch the console off and on: Gowin access clears its running FPGA configuration and USB disappears."


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("command failed: %s\n%s%s" % (args, result.stdout, result.stderr))
    return result.stdout + result.stderr


def find_port():
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x374E, 0x0101)]
    if len(ports) != 1:
        raise ValueError("expected exactly one USB 374e:0101 port; use --port")
    return ports[0]


def loader_path(explicit):
    if explicit:
        return explicit
    for name in ("openFPGALoader", "/opt/homebrew/bin/openFPGALoader", "/usr/local/bin/openFPGALoader"):
        found = shutil.which(name)
        if found:
            return found
    raise ValueError("openFPGALoader not found; use --openfpgaloader")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_files(directory, names):
    directory = Path(directory)
    sums = {}
    for line in (directory / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line)
        if not match or match[2] in sums:
            raise ValueError("invalid or duplicate SHA256SUMS entry")
        sums[match[2]] = match[1].lower()
    files = {}
    for name in names:
        data = (directory / name).read_bytes()
        if digest(data) != sums.get(name):
            raise ValueError("SHA256 mismatch or missing entry: " + name)
        files[name] = data
    return files


def component(value):
    if not value or value in (".", "..") or any(c in value for c in '/\\"\r\n') or any(ord(c) < 32 for c in value):
        raise ValueError("unsafe card path component")
    return value


class Installer:
    """External boundaries are injectable; no hardware is needed by the tests."""
    def __init__(self, args, esp_run=run, fpga_run=run, console=Console,
                 port_finder=find_port, loader_finder=loader_path, sleep=time.sleep,
                 output=print, ask=input):
        self.a = args
        self.esp_run, self.fpga_run, self.console = esp_run, fpga_run, console
        self.port_finder, self.loader_finder = port_finder, loader_finder
        self.sleep, self.output, self.ask = sleep, output, ask
        self.port = self.loader = None

    def esp(self, *args):
        return self.esp_run([sys.executable, "-m", "esptool", "--chip", "esp32", "--port", self.port,
                             "--baud", "115200", *args])

    def cable(self, *args):
        try:
            return self.fpga_run([self.loader, "-c", "gwu2x", *args])
        finally:
            self.output(POWER)

    def detect(self):
        if not re.search(r"\bGW5A-25\b", self.cable("--detect")):
            raise ValueError("Gowin cable did not detect GW5A-25")

    def dump(self, path):
        self.cable("--external-flash", "--dump-flash", "--file-size", str(FLASH), "-o", "0", path)
        data = path.read_bytes()
        if len(data) != FLASH:
            raise ValueError("FPGA dump must be 4 MiB")
        return data

    def confirm(self, description):
        self.output(description)
        if not self.a.yes and self.ask("Type WRITE to continue: ").strip() != "WRITE":
            raise ValueError("cancelled; nothing written")

    def backup_files(self):
        files = checked_files(self.a.backup, BACKUPS)
        if any(len(data) != FLASH for data in files.values()):
            raise ValueError("both backup files must be 4 MiB")
        return files

    def partition(self, expected, work):
        if not expected or len(expected) > 0x1000:
            raise ValueError("invalid shipped partition table size")
        path = work / "partition-now.bin"
        self.esp("read_flash", "0x8000", "0x1000", path)
        actual = path.read_bytes()
        if len(actual) != 0x1000 or actual[:len(expected)] != expected:
            raise ValueError("partition table differs; nothing written")

    def write_esp(self, app, ota):
        self.esp("write_flash", "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep",
                 hex(APP), app, hex(OTA), ota)

    def write_fpga(self, image, work):
        self.cable("--external-flash", "-o", "0", "--verify", image)
        if self.dump(work / "verify.bin") != image.read_bytes():
            raise ValueError("FPGA readback differs from written image")

    def check(self):
        with self.console(self.port) as con:
            self.output(con.boot)
            for attempt in range(5):
                slots = con.command("cart fw slots")
                if "not ready" not in slots:
                    if not re.search(r"cart: fw slots \S+ header .*live .*gb .*gg .*places .*booted", slots):
                        raise ValueError("unrecognized firmware slots response")
                    self.output(slots)
                    break
                if attempt == 4:
                    raise ValueError("firmware slots still not ready")
                self.sleep(3)
            status = con.command("sd st")
            if not re.search(r"sd: st e=0.*games \d+", status):
                raise ValueError("card status unavailable: " + status)
            self.output(status)
        self.detect()

    def backup(self):
        directory = Path(self.a.dir) / datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        directory.mkdir(parents=True, exist_ok=False)
        self.esp("read_flash", "0", hex(FLASH), directory / BACKUPS[0])
        if (directory / BACKUPS[0]).stat().st_size != FLASH:
            raise ValueError("ESP32 backup must be 4 MiB")
        self.detect()
        first = self.dump(directory / BACKUPS[1])
        with tempfile.TemporaryDirectory() as tmp:
            if self.dump(Path(tmp) / "second.bin") != first:
                raise ValueError("FPGA dumps differ; backup is incomplete")
        (directory / "SHA256SUMS").write_text("".join(
            "%s  %s\n" % (digest((directory / n).read_bytes()), n) for n in BACKUPS), encoding="utf-8")
        self.output("Backup: " + str(directory))

    def install(self, work):
        backup = self.backup_files()
        shipped = checked_files(self.a.files, SHIPPED)
        if not 0 < len(shipped["mcu_fw.bin"]) <= APP_SIZE or len(shipped["ota_data_initial.bin"]) != OTA_SIZE:
            raise ValueError("invalid app or otadata size")
        image = flash.compose(backup["fpga.bin"], shipped["fpga-gb.bin"], shipped["fpga-gg.bin"], self.a.live)
        expected = shipped["partition-table.bin"]
        if backup["esp32.bin"][0x8000:0x8000 + len(expected)] != expected:
            raise ValueError("backup partition table differs")
        self.partition(expected, work)
        paths = {}
        for name in ("mcu_fw.bin", "ota_data_initial.bin"):
            paths[name] = work / name
            paths[name].write_bytes(shipped[name])
        target = work / "composed.bin"
        target.write_bytes(image)
        self.confirm("Write ESP32 app 0x10000 and otadata 0x200000; replace all 4 MiB of FPGA flash (live %s)." % self.a.live)
        self.write_esp(paths["mcu_fw.bin"], paths["ota_data_initial.bin"])
        self.detect()
        self.write_fpga(target, work)

    def restore(self, work):
        files = self.backup_files()
        expected = files["esp32.bin"][0x8000:0x9000]
        self.partition(expected, work)
        app, ota, fpga = (work / n for n in ("app.bin", "ota.bin", "fpga.bin"))
        app.write_bytes(files["esp32.bin"][APP:APP + APP_SIZE])
        ota.write_bytes(files["esp32.bin"][OTA:OTA + OTA_SIZE])
        fpga.write_bytes(files["fpga.bin"])
        self.confirm("Restore backup ESP32 app 0x10000 (0x1e0000 bytes), otadata 0x200000 (0x2000 bytes), and all 4 MiB of FPGA flash.")
        self.write_esp(app, ota)
        self.detect()
        self.write_fpga(fpga, work)

    def command(self, command, pattern):
        with self.console(self.port) as con:
            result = con.command(command)
        if not re.search(pattern, result):
            raise ValueError("console command failed: %s\n%s" % (command, result))
        return result

    def addgame(self, work):
        folder = component(self.a.folder)
        name = component(self.a.name or (self.a.rom.stem + ".gb"))
        if not name.lower().endswith(".gb"):
            name += ".gb"
        path = "/Sega GG/%s/%s" % (folder, name)
        data = gg2gb.wrap(self.a.rom.read_bytes(), self.a.title or self.a.rom.stem)
        crc = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
        with self.console(self.port) as con:
            match = re.search(r"cart: running from ota_[01] at [0-9a-fA-F]+: the staged ROM goes to ([0-9a-fA-F]+), the save to", con.boot)
            if not match:
                raise ValueError("boot line has no staging address")
            address = int(match[1], 16)
        if address not in (0x10000, 0x210000) or address + PART > FLASH:
            raise ValueError("unsupported staging address")
        self.confirm("Stage %d bytes in ESP32 flash at %s; import game to %s (crc32 %s)." % (len(data), hex(address), path, crc))
        tmp = "GG" + uuid.uuid4().hex[:8].upper() + ".gb"
        card = "/GAMES/" + tmp
        if len(data) > PART:
            self.command("sd imp begin " + tmp, r"sd: imp begin e=0 path " + re.escape(card))
        for offset in range(0, len(data), PART):
            part = data[offset:offset + PART]
            src = work / "stage.bin"
            src.write_bytes(part)
            if "Hash of data verified" not in self.esp("write_flash", hex(address), src):
                raise ValueError("staging hash verification missing")
            if len(data) <= PART:
                self.command("sd import %d %d %s" % (address, len(part), tmp), r"sd: import e=0 " + re.escape(card))
            else:
                self.command("sd imp part %s %d %d %d" % (card, address, len(part), offset), r"sd: imp part e=0\b")
        if len(data) > PART:
            self.command("sd imp end %s %s" % (card, crc), r"sd: imp end e=0\b")
        self.command('sd mv %s "%s"' % (card, path), r"sd: mv e=0\b")
        result = self.command('sd sum "%s"' % path, r"sd: sum e=0\b")
        match = re.search(r"crc32 ([0-9a-fA-F]{8})\b", result)
        if not match or match[1].lower() != crc:
            raise ValueError("card crc32 mismatch")
        self.output("Imported " + path + " crc32 " + crc)

    def execute(self):
        self.port = self.a.port or self.port_finder()
        if self.a.command != "addgame":
            self.loader = self.loader_finder(self.a.openfpgaloader)
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            if self.a.command in ("check", "backup"):
                getattr(self, self.a.command)()
            else:
                getattr(self, self.a.command)(work)


def parser():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port")
    common.add_argument("--openfpgaloader", metavar="PATH")
    common.add_argument("--files", type=Path, default=Path(__file__).resolve().parent / "files")
    # Accept connection options before or after the command.
    ap = argparse.ArgumentParser(description=__doc__, parents=[common])
    sub = ap.add_subparsers(dest="command", required=True)
    for command in ("check", "backup", "install", "restore", "addgame"):
        child_common = argparse.ArgumentParser(add_help=False)
        child_common.add_argument("--port", default=argparse.SUPPRESS)
        child_common.add_argument("--openfpgaloader", default=argparse.SUPPRESS)
        child_common.add_argument("--files", type=Path, default=argparse.SUPPRESS)
        child = sub.add_parser(command, parents=[child_common])
        if command == "backup":
            child.add_argument("--dir", type=Path, default=Path("backup"))
        if command in ("install", "restore"):
            child.add_argument("--backup", type=Path, required=True)
        if command == "install":
            child.add_argument("--live", choices=("gb", "gg"), default="gb")
        if command in ("install", "restore", "addgame"):
            child.add_argument("--yes", action="store_true")
        if command == "addgame":
            child.add_argument("rom", type=Path)
            child.add_argument("--folder", required=True)
            child.add_argument("--title")
            child.add_argument("--name")
    return ap


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        Installer(args).execute()
    except (OSError, ValueError, RuntimeError, ImportError, EOFError, KeyboardInterrupt) as error:
        print("STOP: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
