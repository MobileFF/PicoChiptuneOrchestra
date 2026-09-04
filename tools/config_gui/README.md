# vgmplay.ini editor (GUI)

A small tkinter desktop app for editing the SD-card config file
([`firmware/vgmplay.ini`](../../firmware/vgmplay.ini) / the copy on your SD
card). Per sound chip: enable/disable, chip-select GPIO, and an optional
per-byte CS-pulse gap. See [docs/circuit.md 1.2](../../docs/circuit.md).

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
**Save As…** straight onto the SD card. **Validate** and every save flag:

- CS GPIO out of range (error)
- CS on a pin the master already uses — 0/1 OLED, 2 skip button, 10/11 slave
  bus, 16–19 SD card (warning)
- two enabled chips sharing a CS GPIO (warning)

The parser matches the firmware (`src/master/src/player_config.c`): section names
ignore case, `-`, `_` and spaces (`[AY-3-8910]` == `[ay8910]`), the same key
aliases work, and comments/unknown keys are reported but not fatal. Rewriting
the file normalises its formatting — hand-written comments are not kept.

## Headless check / tests

```sh
python3 tools/config_gui/vgmplay_config_gui.py --check /path/to/vgmplay.ini   # file or dir
python3 tools/config_gui/test_vgmplay_config_gui.py
```
