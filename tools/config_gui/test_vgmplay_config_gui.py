#!/usr/bin/env python3
"""Headless tests for vgmplay_config_gui's parser/generator/validator.

    python3 tools/config_gui/test_vgmplay_config_gui.py

No display needed -- only the non-GUI functions are exercised. Keeps the
parser in parity with master/src/player_config.c (aliases, name
normalisation) and checks round-tripping and the reserved-pin / duplicate-CS
warnings.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vgmplay_config_gui as g  # noqa: E402

REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))


def test_defaults_roundtrip():
    s, notes = g.parse_ini(g.generate_ini(g.default_rows()))
    assert notes == []
    assert g.rows_from_settings(s) == g.default_rows()


def test_explicit_values_roundtrip():
    rows = g.default_rows()
    rows["ym2151"]["enabled"] = False
    rows["sn76489"]["gap"] = 25
    s, notes = g.parse_ini(g.generate_ini(rows))
    assert notes == []
    assert g.rows_from_settings(s) == rows


def test_firmware_template():
    path = os.path.join(REPO, "firmware", "vgmplay.ini")
    with open(path, encoding="utf-8-sig") as f:
        s, notes = g.parse_ini(f.read())
    assert notes == [], notes
    expected = g.default_rows()
    expected["ay8910"]["gap"] = 120
    expected["ym2151"]["gap"] = 0
    assert g.rows_from_settings(s) == expected
    out = g.generate_ini(expected)
    assert "gap     = 120" in out and "gap     = 0" in out


def test_volume_roundtrip_and_aliases():
    rows = g.default_rows()
    rows["ay8910"]["volume"] = 50
    s, notes = g.parse_ini(g.generate_ini(rows))
    assert notes == []
    assert g.rows_from_settings(s) == rows

    s2, _ = g.parse_ini("[ay8910]\nvol = 75\n")
    assert g.rows_from_settings(s2)["ay8910"]["volume"] == 75


def test_shuffle_roundtrip_and_aliases():
    rows = g.default_rows()
    assert rows[g.PLAYER_SECTION]["shuffle"] is False  # default

    rows[g.PLAYER_SECTION]["shuffle"] = True
    s, notes = g.parse_ini(g.generate_ini(rows))
    assert notes == []
    assert g.rows_from_settings(s) == rows

    # section-name normalisation, and an unknown key inside [player]
    s2, notes2 = g.parse_ini("[ Player ]\nSHUFFLE = on\nwobble = 1\n")
    assert g.rows_from_settings(s2)[g.PLAYER_SECTION]["shuffle"] is True
    assert any("unknown key 'wobble' in [player]" in n for n in notes2)


def test_skip_button_and_preview_roundtrip_and_aliases():
    rows = g.default_rows()
    assert rows[g.PLAYER_SECTION]["skip_button"] is None       # default: use firmware's own
    assert rows[g.PLAYER_SECTION]["preview"] is False           # default
    assert rows[g.PLAYER_SECTION]["preview_seconds"] is None    # default: use firmware's own

    rows[g.PLAYER_SECTION]["skip_button"] = 24
    rows[g.PLAYER_SECTION]["preview"] = True
    rows[g.PLAYER_SECTION]["preview_seconds"] = 15
    s, notes = g.parse_ini(g.generate_ini(rows))
    assert notes == []
    assert g.rows_from_settings(s) == rows

    # key aliases + section-name normalisation
    s2, notes2 = g.parse_ini("[ Player ]\nskip_gpio = 27\nPREVIEW = on\npreview_seconds = 45\n")
    r2 = g.rows_from_settings(s2)[g.PLAYER_SECTION]
    assert r2["skip_button"] == 27 and r2["preview"] is True and r2["preview_seconds"] == 45
    assert notes2 == []

    # out-of-range / bad values are rejected with a note, not silently kept
    _, notes3 = g.parse_ini("[player]\nskip_button = 99\npreview_seconds = 0\n")
    assert any("skip_button" in n and "out of range" in n for n in notes3)
    assert any("preview_seconds" in n for n in notes3)


def test_recursive_roundtrip_and_aliases():
    rows = g.default_rows()
    assert rows[g.PLAYER_SECTION]["recursive"] is False  # default

    rows[g.PLAYER_SECTION]["recursive"] = True
    s, notes = g.parse_ini(g.generate_ini(rows))
    assert notes == []
    assert g.rows_from_settings(s) == rows

    # section-name normalisation, and a bad boolean is rejected with a note
    s2, notes2 = g.parse_ini("[ Player ]\nRECURSIVE = on\n")
    assert g.rows_from_settings(s2)[g.PLAYER_SECTION]["recursive"] is True
    assert notes2 == []

    _, notes3 = g.parse_ini("[player]\nrecursive = maybe\n")
    assert any("bad boolean 'maybe' for recursive" in n for n in notes3)


def test_skip_button_reserved_pin_is_dynamic():
    # Default skip button (GPIO2, unset) still collides with a CS on GPIO2.
    e, w = g.validate({**g.default_rows(),
                       "scc": {"enabled": True, "cs": 2, "gap": None, "volume": None}})
    assert e == [] and any("skip button" in x for x in w)

    # Moving the skip button off GPIO2 frees it up -- no more collision there,
    # but the chip now sharing the NEW skip button GPIO gets flagged instead.
    rows = {**g.default_rows(),
            "scc": {"enabled": True, "cs": 2, "gap": None, "volume": None}}
    rows[g.PLAYER_SECTION] = {**rows[g.PLAYER_SECTION], "skip_button": 24}
    e, w = g.validate(rows)
    assert e == [] and not any("skip button" in x for x in w)

    rows["scc"]["cs"] = 24
    e, w = g.validate(rows)
    assert e == [] and any("GPIO24 collides with skip button" in x for x in w)


def test_aliases_and_normalisation():
    s, _ = g.parse_ini(
        "[SN76489]\nEnable = No\n"
        "[AY-3-8910]\nPin = 7\nGAP_US = 9\n"
        "[k051649]\ncs_gpio = 22\n"
        "[Sega_PCM]\nON = false\n"
    )
    r = g.rows_from_settings(s)
    assert r["sn76489"]["enabled"] is False
    assert r["ay8910"]["cs"] == 7 and r["ay8910"]["gap"] == 9
    assert r["scc"]["cs"] == 22
    assert r["segapcm"]["enabled"] is False


def test_notes_on_bad_input():
    _, notes = g.parse_ini(
        "cs = 5\n"                 # before any section
        "[nope]\ncs = 1\n"         # unknown section
        "[scc]\nenabled = perhaps\ncs = twelve\nzonk = 1\n"
        "line with no equals\n"
    )
    blob = " | ".join(notes)
    assert "outside any [chip] section" in blob
    assert "unknown chip section [nope]" in blob
    assert "bad boolean 'perhaps'" in blob
    assert "bad number 'twelve'" in blob
    assert "unknown key 'zonk'" in blob
    assert "no '='" in blob


def test_validate():
    e, w = g.validate({**g.default_rows(),
                       "scc": {"enabled": True, "cs": 12, "gap": None, "volume": None}})
    assert e == [] and any("also used by" in x for x in w)

    e, _ = g.validate({**g.default_rows(),
                       "scc": {"enabled": True, "cs": 40, "gap": None, "volume": None}})
    assert any("out of range" in x for x in e)

    _, w = g.validate({**g.default_rows(),
                       "scc": {"enabled": True, "cs": 18, "gap": None, "volume": None}})
    assert any("SD card" in x for x in w)

    # a disabled chip does not trip collision/reserved warnings
    _, w = g.validate({**g.default_rows(),
                       "scc": {"enabled": False, "cs": 12, "gap": None, "volume": None}})

    e, _ = g.validate({**g.default_rows(),
                       "scc": {"enabled": True, "cs": 22, "gap": None, "volume": 300}})
    assert any("out of range (0-255)" in x for x in e)
    assert not any("also used by" in x for x in w)


def test_resolve_config_path():
    with tempfile.TemporaryDirectory() as d:
        # a plain file path is returned unchanged
        f = os.path.join(d, "whatever.ini")
        open(f, "w").close()
        assert g.resolve_config_path(f) == f

        # directory, nothing there yet -> points at the (missing) vgmplay.ini
        assert g.resolve_config_path(d) == os.path.join(d, "vgmplay.ini")

        # only a suffixed one -> that, case-insensitively
        open(os.path.join(d, "VGMPlay_SCC.ini"), "w").close()
        open(os.path.join(d, "vgmplay_zzz.ini"), "w").close()
        assert os.path.basename(g.resolve_config_path(d)) == "VGMPlay_SCC.ini"

        # exact vgmplay.ini wins over any suffixed variant
        open(os.path.join(d, "vgmplay.ini"), "w").close()
        assert os.path.basename(g.resolve_config_path(d)) == "vgmplay.ini"


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
