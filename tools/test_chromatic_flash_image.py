import pytest

from chromatic_flash_image import (FLASH, FULL_TOP, GAME_BASE, GAME_END, REC_BASE, REC_HDR, SLOT_MAX, compose, describe,
                                   header, headers, slot_base, slot_of, SELECTOR_A, SELECTOR_B)


def design(n, fill):
    return b"\xFF" * 16 + b"\xA5\xC3" + bytes([fill]) * (n - 18)


def base_dump():
    img = bytearray(b"\xFF" * FLASH)
    img[REC_HDR:REC_HDR + 64] = header(REC_BASE)
    img[REC_BASE:GAME_BASE] = b"\x11" * (GAME_BASE - REC_BASE)
    return bytes(img)


def test_header_matches_the_unit():
    # the live header read from the unit on 2026-11-16 (place 0x300 -> 0x34B000)
    h = header(0x34B000)
    assert h[24:28] == b"\xDE" * 4 and h[34:36] == b"\xA5\xC3"
    assert h[36:52] == bytes.fromhex("06000000000128 1b 6e800000 0034b000".replace(" ", ""))
    assert h[54:] == b"\xFF" * 10


def test_slots():
    assert slot_base("gb", 738165) == 0x34B000
    assert slot_base("gg", 738165) == 0x27B000
    assert GAME_END == 0x25E000
    assert slot_of(0x34B000) == "gb" and slot_of(0x27B000) == "gg" and slot_of(0x100000) is None
    with pytest.raises(ValueError):
        slot_base("gb", SLOT_MAX + 1)


def test_compose_and_describe():
    gb, gg = design(700000, 0x22), design(740000, 0x33)
    img = compose(base_dump(), gb, gg, "gg")
    assert len(img) == FLASH
    assert img[REC_BASE:GAME_BASE] == b"\x11" * (GAME_BASE - REC_BASE)
    assert img[GAME_BASE:GAME_END] == b"\xFF" * (GAME_END - GAME_BASE)
    a, b = slot_base("gb", len(gb)), slot_base("gg", len(gg))
    assert img[a:a + len(gb)] == gb and img[b:b + len(gg)] == gg
    hs = headers(img)
    assert (REC_HDR, "live", REC_BASE) in hs and (FULL_TOP, "live", SELECTOR_A) in hs and len(hs) == 2
    text = describe(img)
    assert "free full header places: 30" in text and "boots: slot gg at %06x" % b in text
    assert "boots: slot gb" in describe(compose(base_dump(), gb, gg, "gb"))


def test_compose_refuses_bad_input():
    with pytest.raises(ValueError):
        compose(b"\xFF" * FLASH, design(1000, 1), design(1000, 2), "gb")       # no recovery header
    with pytest.raises(ValueError):
        compose(base_dump(), b"\x00" * 1000, design(1000, 2), "gb")            # not a bitstream


def test_invalidated_and_boot_order():
    img = bytearray(compose(base_dump(), design(1000, 1), design(1000, 2), "gb"))
    img[FULL_TOP + 34:FULL_TOP + 36] = b"\x00\x00"
    low = FULL_TOP - 0x40
    img[low:low + 64] = header(slot_base("gg", 1000))
    text = describe(bytes(img))
    assert "header 780 invalidated" in text and "boots: slot gg" in text and "free full header places: 29" in text


def test_direct_and_chain_scan():
    gb, gg = design(1000, 1), design(1000, 2)
    direct = compose(base_dump(), gb, gg, "gb", "direct")
    assert (FULL_TOP, "live", slot_base("gb", len(gb))) in headers(direct)
    img = bytearray(compose(base_dump(), gb, gg, "gb"))
    assert "free level-2 header places: 30" in describe(img)
    at = SELECTOR_A + FULL_TOP - 64
    img[at:at + 64] = header(slot_base("gg", len(gg)))
    assert "boots: slot gg" in describe(img)
    assert "free level-2 header places: 29" in describe(img)
    img[SELECTOR_A:SELECTOR_B] = b"\xFF" * 4096
    assert "boots: nothing" in describe(img)
