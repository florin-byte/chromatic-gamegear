"""Hardware-free acceptance tests using injected process and console boundaries."""
from pathlib import Path
import tempfile
import unittest
import zlib
from unittest.mock import patch

import chromatic_install as ci


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.files = self.root / "files"
        self.backup = self.root / "backup"
        self.files.mkdir()
        self.backup.mkdir()
        self.pt = b"P" * 0x1000
        self.esp_image = bytearray(b"\xff" * ci.FLASH)
        self.esp_image[0x8000:0x9000] = self.pt
        self.fpga_image = bytearray(b"\xff" * ci.FLASH)
        self.fpga_image[ci.flash.REC_HDR:ci.flash.REC_HDR + 64] = ci.flash.header(ci.flash.REC_BASE)
        self.fpga_image[0x800:0x900] = b"R" * 256
        self.shipped = dict(zip(ci.SHIPPED, (b"APP", b"\xff" * ci.OTA_SIZE, self.pt,
                                             b"\xa5\xc3" + b"B" * 99, b"\xa5\xc3" + b"G" * 199)))
        self.save(self.files, self.shipped)
        self.save(self.backup, dict(zip(ci.BACKUPS, (self.esp_image, self.fpga_image))))
        self.events = []
        self.written = None
        self.different = False
        self.dump_count = 0
        self.bad_crc = False
        self.not_ready = 0
        self.stage_address = 0x10000
        self.boot = "diag: boot fw=1.1.5-fr3m\ncart: running from ota_1 at 210000: the staged ROM goes to 010000, the save to 208000"

    def save(self, directory, files):
        for name, data in files.items():
            (directory / name).write_bytes(data)
        (directory / "SHA256SUMS").write_text("".join("%s  %s\n" % (ci.digest(data), n) for n, data in files.items()))

    def esp(self, args):
        self.events.append(("esp", args))
        if "read_flash" in args:
            at = int(args[-3], 0)
            data = self.pt if at == 0x8000 else self.esp_image
            Path(args[-1]).write_bytes(data)
        else:
            if "--flash_mode" not in args:
                self.assertEqual(int(args[-2], 0), self.stage_address)
        return "Hash of data verified"

    def fpga(self, args):
        self.events.append(("fpga", args))
        if "--detect" in args:
            return "GW5A-25"
        if "--dump-flash" in args:
            self.dump_count += 1
            data = self.written if self.written is not None else bytes(self.fpga_image)
            if self.different and self.dump_count == 2:
                data = b"X" + data[1:]
            Path(args[-1]).write_bytes(data)
        if "--verify" in args:
            self.written = Path(args[-1]).read_bytes()
        return "ok"

    def console(self, port):
        outer = self
        class Fake:
            boot = outer.boot
            def __enter__(self):
                outer.events.append(("console", "open"))
                return self
            def __exit__(self, *args):
                pass
            def command(self, command):
                outer.events.append(("console", command))
                if command == "cart fw slots":
                    if outer.not_ready:
                        outer.not_ready -= 1
                        return "cart: fw slots: not ready: try again (scan 0)"
                    return "cart: fw slots rf header 700 live 1 gb 343000 gg 27b000 places 28 booted 1"
                if command == "sd st":
                    return "sd: st e=0 state READY, games 202 (shown 22)"
                if command.startswith("sd sum"):
                    return "sd: sum e=0 crc32 " + ("00000000" if outer.bad_crc else outer.crc)
                if command.startswith("sd import"):
                    return "sd: import e=0 /GAMES/" + command.split()[-1] + " ok"
                if command.startswith("sd imp begin"):
                    return "sd: imp begin e=0 path /GAMES/" + command.split()[-1]
                if command.startswith("sd imp"):
                    return "sd: imp " + command.split()[2] + " e=0"
                return "sd: mv e=0"
        return Fake()

    def tool(self, command, extra=(), ask=lambda _: "WRITE"):
        args = ci.parser().parse_args(["--port", "fake", "--files", str(self.files), command, *extra])
        return ci.Installer(args, self.esp, self.fpga, self.console,
                            loader_finder=lambda _: "loader", sleep=lambda _: None,
                            output=lambda _: None, ask=ask)

    def install(self):
        self.tool("install", ["--backup", str(self.backup), "--yes"]).execute()

    def assert_no_writes(self):
        self.assertFalse(any("write_flash" in a or "--verify" in a for _, a in self.events))

    def test_install_missing_backup(self):
        with self.assertRaises(OSError):
            self.tool("install", ["--backup", str(self.root / "missing")]).execute()
        self.assert_no_writes()

    def test_install_wrong_backup_sum(self):
        (self.backup / "esp32.bin").write_bytes(b"wrong")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            self.install()
        self.assert_no_writes()

    def test_install_wrong_shipped_sum(self):
        (self.files / "fpga-gb.bin").write_bytes(b"wrong")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            self.install()
        self.assert_no_writes()

    def test_install_partition_mismatch(self):
        self.pt = b"Q" * 0x1000
        with self.assertRaisesRegex(ValueError, "partition table differs"):
            self.install()
        self.assert_no_writes()

    def test_install_no_recovery_header(self):
        self.save(self.backup, {"esp32.bin": self.esp_image, "fpga.bin": b"\xff" * ci.FLASH})
        with self.assertRaisesRegex(ValueError, "no recovery header"):
            self.install()
        self.assert_no_writes()

    def test_install_order_and_compose(self):
        self.install()
        self.assertEqual(self.written, ci.flash.compose(bytes(self.fpga_image), self.shipped["fpga-gb.bin"], self.shipped["fpga-gg.bin"], "gb"))
        kinds = [k for k, _ in self.events]
        self.assertEqual(kinds, ["esp", "esp", "fpga", "fpga", "fpga"])
        self.assertIn("read_flash", self.events[0][1])
        self.assertIn("--verify", self.events[-2][1])

    def test_backup_order_and_sums(self):
        dest = self.root / "new-backups"
        self.tool("backup", ["--dir", str(dest)]).execute()
        saved = next(dest.iterdir())
        self.assertEqual(ci.checked_files(saved, ci.BACKUPS)["fpga.bin"], self.fpga_image)
        self.assertEqual([k for k, _ in self.events], ["esp", "fpga", "fpga", "fpga"])
        self.assertEqual(self.dump_count, 2)

    def test_backup_dumps_differ(self):
        self.different = True
        dest = self.root / "bad-backups"
        with self.assertRaisesRegex(ValueError, "dumps differ"):
            self.tool("backup", ["--dir", str(dest)]).execute()
        self.assertFalse(list(dest.rglob("SHA256SUMS")))

    def test_restore_regions_and_order(self):
        writes = []
        original = self.esp
        def esp(args):
            if "write_flash" in args:
                writes.extend([Path(args[-3]).read_bytes(), Path(args[-1]).read_bytes()])
            return original(args)
        tool = self.tool("restore", ["--backup", str(self.backup), "--yes"])
        tool.esp_run = esp
        tool.execute()
        self.assertEqual(writes, [bytes(self.esp_image[ci.APP:ci.APP + ci.APP_SIZE]), bytes(self.esp_image[ci.OTA:ci.OTA + ci.OTA_SIZE])])
        self.assertEqual(self.written, self.fpga_image)
        self.assertEqual([k for k, _ in self.events], ["esp", "esp", "fpga", "fpga", "fpga"])

    def game(self, size):
        # Synthetic bytes exist only in a temporary directory; no ROM assets are shipped.
        rom = self.root / "synthetic.gg"
        rom.write_bytes(b"\x00" * size)
        data = ci.gg2gb.wrap(rom.read_bytes(), rom.stem)
        self.crc = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
        self.tool("addgame", [str(rom), "--folder", "Tests", "--yes"]).execute()
        return [a for k, a in self.events if k == "console" and isinstance(a, str)]

    def test_addgame_small_boot_address(self):
        commands = self.game(16384)
        self.assertTrue(any(c.startswith("sd import 65536 65536 ") for c in commands))
        self.assertFalse(any(k == "fpga" for k, _ in self.events))

    def test_addgame_two_parts(self):
        commands = self.game(ci.PART)
        parts = [c for c in commands if c.startswith("sd imp part")]
        self.assertEqual(len(parts), 2)
        self.assertTrue(parts[0].endswith("65536 1048576 0"))
        self.assertTrue(parts[1].endswith("65536 1048576 1048576"))
        self.assertTrue(any(c.startswith("sd imp end") for c in commands))

    def test_addgame_wrong_crc(self):
        self.bad_crc = True
        with self.assertRaisesRegex(ValueError, "crc32 mismatch"):
            self.game(16384)

    def test_check_not_ready_retries(self):
        self.not_ready = 2
        self.tool("check").execute()
        self.assertEqual(sum(a == "cart fw slots" for _, a in self.events), 3)
        self.assertEqual(self.events[-1][0], "fpga")

    def test_cancel_before_writes(self):
        with self.assertRaisesRegex(ValueError, "cancelled"):
            self.tool("install", ["--backup", str(self.backup)], ask=lambda _: "NO").execute()
        self.assert_no_writes()

    def test_unsafe_card_paths(self):
        for name in ("../bad", 'bad"', "bad\ncommand", ".."):
            with self.assertRaises(ValueError):
                ci.component(name)

    def test_port_matches_vid_and_pid(self):
        from types import SimpleNamespace, ModuleType
        ports = ModuleType("serial.tools.list_ports")
        ports.comports = lambda: [SimpleNamespace(vid=0x374E, pid=0x0101, device="right"), SimpleNamespace(vid=0x374E, pid=2, device="wrong")]
        serial = ModuleType("serial")
        serial_tools = ModuleType("serial.tools")
        serial_tools.list_ports = ports
        with patch.dict("sys.modules", {"serial": serial, "serial.tools": serial_tools, "serial.tools.list_ports": ports}):
            self.assertEqual(ci.find_port(), "right")


if __name__ == "__main__":
    unittest.main()
