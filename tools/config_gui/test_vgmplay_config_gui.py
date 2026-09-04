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
    expected["ay8910"]["gap"] = 80
    expected["ym2151"]["gap"] = 120
    assert g.rows_from_settings(s) == expected
    out = g.generate_ini(expected)
    assert "gap     = 80" in out and "gap     = 120" in out


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
                       "scc": {"enabled": True, "cs": 12, "gap": None}})
    assert e == [] and any("also used by" in x for x in w)

    e, _ = g.validate({**g.default_rows(),
                       "scc": {"enabled": True, "cs": 40, "gap": None}})
    assert any("out of range" in x for x in e)

    _, w = g.validate({**g.default_rows(),
                       "scc": {"enabled": True, "cs": 18, "gap": None}})
    assert any("SD card" in x for x in w)

    # a disabled chip does not trip collision/reserved warnings
    _, w = g.validate({**g.default_rows(),
                       "scc": {"enabled": False, "cs": 12, "gap": None}})
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
