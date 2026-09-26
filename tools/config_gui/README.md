# vgmplay.ini editor (GUI)

A small tkinter desktop app for editing the SD-card config file
([`firmware/vgmplay.ini`](../../firmware/vgmplay.ini) / the copy on your SD
card). Per sound chip -- including the optional second SN76489 for VGM's
"Dual Chip Support" (see [docs/circuit.md 1.3](../../docs/circuit.md)) --
enable/disable, chip-select GPIO, and an optional per-byte CS-pulse gap. Plus
general playback settings: shuffle, the skip button's GPIO, preview mode (cut
every song short after N seconds), recursive mode (walk every subfolder
instead of just the start folder), and the SD-card-relative folder to start
scanning from. See [docs/circuit.md 1.2](../../docs/circuit.md).

## Run

```sh
python3 tools/config_gui/vgmplay_config_gui.py                    # empty / defaults
python3 tools/config_gui/vgmplay_config_gui.py /path/to/vgmplay.ini
python3 tools/config_gui/vgmplay_config_gui.py /media/SD_CARD      # a directory: picks
                                                                 # vgmplay.ini, else the
                                                                 # first vgmplay*.ini
```

The firmware loads `vgmplay.ini` if present, otherwise the
alphabetically-first `vgmplay*.ini` on the card -- so you can name a card's
config `vgmplay_scc.ini`, `vgmplay_outrun.ini` etc. and tell at a glance
what it's for. Passing a directory here applies the same rule.

Python 3.8+, standard library only (tkinter — ships with CPython; on some
Linux distros it's a separate `python3-tk` package). Works on Windows, macOS
and Linux.

In the window: tick **Enabled**, set **CS GPIO** (0–28) and optionally
**Gap (µs)** (leave blank to use the firmware default), then **Save** /
**Save As…** straight onto the SD card. Above the chip table: **Shuffle**,
**Skip button GPIO** (blank = firmware default GPIO2 — set this for a clone
board whose built-in button is wired elsewhere, e.g. a "USR" button on
GPIO24), **Preview mode** and **Preview seconds** (blank = firmware default
30; cuts every song short and advances, as if the skip button had been
pressed — for auditioning a whole card quickly), **Recursive** (walks every
subfolder under the start folder, playing each one's files in turn — shuffle
still applies per folder, not globally), and **Start folder** (blank = SD
card root; a card-relative path like `GAMES/Sega` to scan from there instead
— a leading/trailing slash or a `0:` drive prefix, if typed, is stripped
automatically; combine with Recursive to walk everything under just that one
folder). Its **Browse…** button opens a folder picker rooted at the SD card
and fills in the path relative to it automatically — it needs to know where
the SD card is first, so open this vgmplay.ini from the card (or Save/Save
As onto it) before using Browse; picking a folder outside the card is
rejected with an error. **Validate** and every save flag:

- CS GPIO out of range (error)
- CS on a pin the master already uses — 0/1 OLED, the skip button's GPIO
  (wherever it's currently set, default 2), 10/11 slave bus, 16–19 SD card
  (warning)
- two enabled chips sharing a CS GPIO (warning)
- skip_button/preview_seconds out of range (error)
- start folder longer than the firmware's buffer (error)

The parser matches the firmware (`src/master/src/player_config.c`): section names
ignore case, `-`, `_` and spaces (`[AY-3-8910]` == `[ay8910]`), the same key
aliases work, and comments/unknown keys are reported but not fatal. Rewriting
the file normalises its formatting — hand-written comments are not kept.

## Headless check / tests

```sh
python3 tools/config_gui/vgmplay_config_gui.py --check /path/to/vgmplay.ini   # file or dir
python3 tools/config_gui/test_vgmplay_config_gui.py
```
