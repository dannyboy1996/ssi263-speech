# The Blazie emulator

A Braille Lite 2000 you can use from a PC keyboard: Blazie's own firmware on the emulated board
(`../../csrc/blazie`), with the emulated SSI-263 (`../../csrc/ssi263.c`) as its voice. Unlike the screen-reader
drivers, the unit is not put into speech-box mode: it boots to its own main menu and behaves as the unit does,
including the channel left open (hiss or whine) until the firmware clicks it off.

| File | What it does |
| --- | --- |
| `chords.h`, `chords.c` | A braille chord from separate key presses: sent when the last key of the chord comes up. Portable. |
| `emu_unit.h`, `emu_unit.c` | One unit running in real time: create from firmware + state, render 16-bit PCM, take chords. Portable. |
| `main_win.c` | The Windows shell: the window, the menu (unit, idle channel sound, help), the keyboard map, waveOut. |
| `build_app.py` | Builds `blazie_emu.exe` and the two test programs into `nvda/dist/blazie-emu/` (w64devkit, x64, static). |
| `test_chords.c` | The chord logic. |
| `test_emu_unit.c` | The unit headless: the boot greeting is heard, a chord is answered (a no-chord run is the control), faster than real time. |

## Keys

While the window is in front: F D S = dots 1 2 3, J K L = dots 4 5 6, the space bar = space, A or ; = the advance
bar. There are no cursor-routing keys and no dots 7 and 8: the Braille Lite 2000 has none. Every other key goes to
Windows (Alt opens the menu). The keys can be changed in `blazie_emu.ini` beside the program, section `[keys]`
(`dot1=F`, ..., `space=space`, `advance=A ;`).

The key port: dot 1 = bit 0 .. dot 6 = bit 5, space = bit 6, and bit 7 for the advance bar (silent in the main
menu, as moving a display would be; still to be confirmed on the unit).

## Firmware

Never in the repository. A release puts `firmware\` beside the program (`BL2ENG.BNS` + `bl2_2003_warm.state`, and
`spanish\BL2SPA.BNS` + `spanish\bl2spa_fresh.state`, the state the Spanish driver uses; the "warm" one has
speech off). Run from the source tree, the program finds
`firmware/blazie/` itself; `firmware_dir=` in `[unit]` overrides both.

## What the unit keeps

As a real unit keeps its battery-backed RAM and file flash while switched off, the program saves them on exit (and
when you switch units) to `%APPDATA%\ssi263-speech\blazie-emu\english.state` or `spanish.state`, and starts from
them next time. The first time, and after Unit > Back to the factory state, it starts from the shipped state.

## Not yet

- The Type 'n Speak (a QWERTY unit): a different board -- the chip at 90h-94h, other key ports. Being mapped.
- Linux and Android shells (the two portable files are ready for them).
