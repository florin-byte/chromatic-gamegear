import struct
import zlib

import pytest

from gg2gb import MAGIC, MAX_TOTAL, PREFIX, main, title_from_name, wrap


def test_title_from_name_basic():
    assert title_from_name("my game").decode() == "MY GAME"
    assert title_from_name("My!   Game#2").decode() == "MY GAME 2"
    assert title_from_name("A" * 20).decode("ascii") == "A" * 15
    assert title_from_name("   ").decode() == "GAME GEAR"
    assert title_from_name("  foo  ").decode() == "FOO"
    assert all(0x20 <= b <= 0x7E for b in title_from_name("Sonic_the_Hedgehog (World)"))


def test_wrap_invalid_roms():
    with pytest.raises(ValueError, match="empty ROM"):
        wrap(b"")
    with pytest.raises(ValueError, match="ROM too big"):
        wrap(b"\xFF" * (MAX_TOTAL - PREFIX + 1))


def test_wrap_with_copier_header():
    rom = bytes(range(256)) * 128  # 32 KiB
    wrapped = wrap(b"\xAA" * 512 + rom, "TestRom")
    assert wrapped[PREFIX:PREFIX + len(rom)] == rom
    assert struct.unpack("<I", wrapped[0x158:0x15C])[0] == len(rom)
    assert len(wrapped) == 0x10000


def test_wrap_basic_structure_and_checksum():
    rom = b"\x00\x01\x02\x03" * 100
    wrapped = wrap(rom, "Demo")
    assert wrapped[0x100:0x104] == bytes([0x00, 0xC3, 0x50, 0x01])
    assert wrapped[0x134:0x144] == b"DEMO".ljust(16, b"\x00")
    assert wrapped[0x147] == 0x1B and wrapped[0x149] == 0x03
    assert wrapped[0x150:0x158] == MAGIC
    assert struct.unpack("<I", wrapped[0x158:0x15C])[0] == len(rom)
    assert struct.unpack("<I", wrapped[0x15C:0x160])[0] == zlib.crc32(rom) & 0xFFFFFFFF
    calc = 0
    for i in range(0x134, 0x14D):
        calc = (calc - wrapped[i] - 1) & 0xFF
    assert wrapped[0x14D] == calc
    body = bytearray(wrapped)
    body[0x14E:0x150] = bytes(2)
    assert struct.unpack(">H", wrapped[0x14E:0x150])[0] == sum(body) & 0xFFFF
    assert wrapped[PREFIX + len(rom):] == b"\xFF" * (len(wrapped) - PREFIX - len(rom))


@pytest.mark.parametrize("size,total,code", [
    (32, 0x10000, 1), (0x8000, 0x10000, 1), (0x8001, 0x20000, 2),
    (0x40000, 0x80000, 4), (0x100000, 0x200000, 6),
])
def test_wrap_sizes_match_firmware_rule(size, total, code):
    # Firmware (sdc_rom_fit): rom size code <= 7, banks = 2 << code (16 KiB each).
    wrapped = wrap(b"\x55" * size, "Size")
    assert len(wrapped) == total
    assert wrapped[0x148] == code <= 7
    assert (2 << wrapped[0x148]) * 0x4000 == total


def test_main_writes_file(tmp_path, capsys):
    rom_file = tmp_path / "sample.gg"
    rom_file.write_bytes(b"\x01\x02\x03" * 200)
    out_dir = tmp_path / "out"
    assert main([str(rom_file), "-o", str(out_dir)]) == 0
    assert (out_dir / "sample.gg.gb").read_bytes() == wrap(rom_file.read_bytes(), "sample")


def test_main_error_handling(tmp_path, capsys):
    bad_file = tmp_path / "doesnotexist.gg"
    assert main([str(bad_file)]) == 1
    assert str(bad_file) in capsys.readouterr().err
