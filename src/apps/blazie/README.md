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
| `main_win.c` | The Windows shell: the window, the menu (unit, idle channel sound, serial port, help), the keyboard, waveOut. |
| `serial_win.c`, `.h` | The unit's serial port on a Windows COM port: the port list, and a thread moving bytes and setting the port as the firmware programs it. The portable half is `../../csrc/blazie/bl_serial.c`. |
| `tns_keymap_win.c`, `.h` | A Windows key to the Type 'n Speak's key code (measured on the running firmware). |
| `build_app.py` | Builds `blazie_emu.exe` and the test programs into `nvda/dist/blazie-emu/` (w64devkit, x64, static). |
| `test_chords.c` | The chord logic. |
| `test_emu_unit.c` | The unit headless: the boot greeting is heard, a chord is answered (a no-chord run is the control), faster than real time. |
| `test_serial.c` | The serial port plugged in, headless: the storage handshake answered from the far end, on every unit (below); built with the receive path cut, it must fail. |
| `test_serial_win.c` | `serial_win.c` end to end, a named pipe standing in for the COM port and this program for WinDisk; built with the receive path cut, it must fail. |

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

## The serial port: WinDisk, PCDISK, a terminal

Settings > Serial port plugs the unit's serial port (its RS-232 port) into a COM port of this PC. The menu lists
the COM ports Windows has at the moment you open it, each with its name from Device Manager, for example "COM10,
com0com - serial port emulator", and "None (not connected)" first; the one in use is checked. The choice is kept
in `blazie_emu.ini` (`[serial]`, `port=COM10`) and used again next time; if that port is gone, the program says
so and starts with the serial port unplugged. The window's title says which port the unit is on ("Braille Lite
2000 (English), serial port on COM10").

**WinDisk on the same PC** needs a virtual null-modem cable: a pair of COM ports wired to each other. com0com (a
free driver) makes one, for example COM10 and COM11. Keep its default wiring (each side's RTS to the other's CTS,
DTR to DSR and DCD); its "emulate baud rate" option is not needed. Choose one end in the emulator (Settings >
Serial port > COM10) and the other end in WinDisk (COM11). A real null-modem cable to another PC works the same
way with the real COM port.

Then use the unit as you would with the disk drive or WinDisk: on the Braille Lite, s-chord (Storage) and a
command letter -- d for a directory, l to load a file, s to save one -- or t-chord in the Files menu to send or
receive several files; on the Type 'n Speak, F8 or Alt+S. The unit finds the far end by itself: it switches its
serial port on, calls at 19200 bit/s, and when nothing answers says "storage device missing". For a terminal
program, turn the serial port on in the unit's Status menu (dots 3-4 chord, f, y); it runs at the unit's own
settings, 9600 bit/s, 8 data bits, no parity, software handshake unless you change them there.

What is carried, measured on the running firmware (`../../csrc/blazie/bl_serial.h`): the unit's RS-232 port, at
the rate, data bits and parity the firmware programs, changed on the COM port in step with the bytes (the storage
commands switch to 19200 and back). DTR is on while the unit's serial port is on; RTS follows the unit's own
handshake line. The unit's XON and XOFF go through as they are, and so do the far end's: Windows' own flow
control is off. The far end's CTS, DSR and DCD are not watched (the unit always sees them on). The disk drive's
own port on the Braille Lite is not carried: WinDisk and PCDISK talk over the RS-232 port.

Not yet tried: WinDisk itself against the emulator (the machine it was built on has no com0com). The tests answer
the unit's call from a program instead: the unit calls with XON ENQ at 19200 8N1, the far end answers ACK, the unit
answers 'C' and says "storage"; the directory command then goes out as ENQ, "d", carriage return.

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

- Linux and Android shells (the portable files are ready for them; the serial port would be a tty there).
