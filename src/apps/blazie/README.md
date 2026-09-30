# The Blazie emulator

A Braille Lite 2000 or a Type 'n Speak you can use from a PC keyboard: Blazie's own firmware on the emulated board
(`../../csrc/blazie`: `bl_board.c`, `tns_board.c`), with the emulated SSI-263 (`../../csrc/ssi263.c`) as its
voice. Unlike the screen-reader
drivers, the unit is not put into speech-box mode: it boots to its own main menu and behaves as the unit does,
including the channel left open (hiss or whine) until the firmware clicks it off.

| File | What it does |
| --- | --- |
| `chords.h`, `chords.c` | A braille chord from separate key presses: sent when the last key of the chord comes up. Portable. |
| `emu_unit.h`, `emu_unit.c` | One unit running in real time: create from firmware + state, render 16-bit PCM, take chords. Portable. |
| `main_win.c` | The Windows shell: the window, the menu (unit, idle channel sound, help), the keyboard, waveOut. |
| `tns_keymap_win.c`, `.h` | A Windows key to the Type 'n Speak's key code (measured on the running firmware). |
| `build_app.py` | Builds `blazie_emu.exe` and the two test programs into `nvda/dist/blazie-emu/` (w64devkit, x64, static). |
| `test_chords.c` | The chord logic. |
| `test_emu_unit.c` | The unit headless: the boot greeting is heard, a chord is answered (a no-chord run is the control), faster than real time. |

## Keys

**Braille Lite**, while the window is in front: F D S = dots 1 2 3, J K L = dots 4 5 6, the space bar = space, A or ; = the advance
bar. There are no cursor-routing keys and no dots 7 and 8: the Braille Lite 2000 has none. Every other key goes to
Windows (Alt opens the menu). The keys can be changed in `blazie_emu.ini` beside the program, section `[keys]`
(`dot1=F`, ..., `space=space`, `advance=A ;`).

**Type 'n Speak**: the whole keyboard is the unit's, Alt and the function keys included. **Alt+Shift+F** (or F11)
always opens this program's menu, whichever unit is running (the unit's held keys are let go first). A key goes to the unit as it goes down and again as it comes up (bit 7 = down), as the unit's own keyboard
sends them; auto-repeat is left to the unit.

The Braille Lite's key port: dot 1 = bit 0 .. dot 6 = bit 5, space = bit 6, and bit 7 for the advance bar (silent in the main
menu, as moving a display would be; still to be confirmed on the unit).

## Settings

Settings > Idle channel (hiss, whine or silent) and Settings > Sample rate (11025 to 48000 Hz; the unit restarts at
the new rate with its memory kept). The unit's own speech settings -- rate, pitch, inflection, volume -- are set on
the unit, with its own keys, as on the real one.

Settings > Quick key response (off by default): after a key the firmware works before it speaks -- speech 283 ms after
a chord in the English Braille Lite's main menu, 107 ms in the Spanish one, 243 ms after a Type 'n Speak key (chip time,
test_emu_unit's "key latency"), the unit's own pace at its 6.144 MHz clock.  On, the CPU runs 8 times faster from a key
until the first spoken phoneme loads: 56, 83 and 51 ms.  The phonemes and every register value are unchanged; only the
wait before the first one is shorter than on the real unit.

Sound: four blocks of 10 ms (`[sound] block_ms` in `blazie_emu.ini`, 5-20).  A key's speech plays behind the blocks
already queued, so 20 ms blocks (0.6.0) added 60-80 ms; 10 ms blocks add 30-40.  Raise it if the sound breaks up.

## Firmware

Never in the repository. A release puts `firmware\` beside the program (`BL2ENG.BNS` + `bl2_2003_warm.state`,
`tns\TNSENG.TNS` and `tns\TNSSPA.TNS` (the September 2000 revision; the Type 'n Speak needs no state), and
`spanish\BL2SPA.BNS` + `spanish\bl2spa_fresh.state`, the state the Spanish driver uses; the "warm" one has
speech off). Run from the source tree, the program finds
`firmware/blazie/` itself; `firmware_dir=` in `[unit]` overrides both.

## What the unit keeps

As a real unit keeps its battery-backed RAM and file flash while switched off, the program saves them on exit (and
when you switch units) to `%APPDATA%\ssi263-speech\blazie-emu\english.state` or `spanish.state`, and starts from
them next time. The first time, and after Unit > Back to the factory state, it starts from the shipped state; the
Type 'n Speak starts cold -- the program holds Ctrl+Alt+Del at power-on, the unit's own reset to its defaults
(without it blank RAM leaves the volume at 0), and the unit asks to initialise its flash: y, then y (Spanish: s).

## Not yet

- Linux and Android shells (the two portable files are ready for them).
