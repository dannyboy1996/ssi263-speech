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
| `main_win.c` | The Windows shell: the window, the menu (firmware, idle channel, keep open, pop and tick, sample rate, serial port, help), the keyboard, waveOut. |
| `serial_win.c`, `.h` | The unit's serial port on a Windows COM port: the port list, and a thread moving bytes and setting the port as the firmware programs it. The portable half is `../../csrc/blazie/bl_serial.c`. |
| `tns_keymap_win.c`, `.h` | A Windows key to the Type 'n Speak's key code (measured on the running firmware). |
| `build_app.py` | Builds `blazie_emu.exe` and the test programs into `nvda/dist/blazie-emu/` (w64devkit, x64, static). |
| `test_chords.c` | The chord logic. |
| `test_emu_unit.c` | The unit headless: the boot greeting is heard, a chord is answered (a no-chord run is the control), faster than real time. |
| `test_clock.c` | The clock controller alone, then the English Braille Lite and Type 'n Speak setting and reading the time and date with their own commands, the clock going on and kept over a switch-off; and i-chord held through p-chord l's restart. `TEST_CLOCK_BREAK` / `TEST_CLOCK_HOLD_BREAK` put one bug back for run_tests' must-fail controls. |
| `test_flash.c` | The file flash: the Type 'n Speak's ID check passes and its flash is initialised, the erase takes 32 s with the firmware's clicks, the flash kept across a save and restart; the Braille Lite's reset erases the same way, and a file moved to flash lands in the 2 MB and survives a restart. `test_flash_break.exe` (the old flash put back) and `--break=instant|persist` must fail. |
| `test_idle.c` | The idle channel against Tomi's unit (the noise's level at volumes 1, 6 and 15, keep open off/until/always, the pop, the click-off, the tick); `--break=...` puts one bug back for run_tests' must-fail controls. |
| `test_serial.c` | The serial port plugged in, headless: the storage handshake answered from the far end, on every unit (below); built with the receive path cut, it must fail. |
| `test_serial_win.c` | `serial_win.c` end to end, a named pipe standing in for the COM port and this program for WinDisk; built with the receive path cut, it must fail. |
| `blazie_files.c` | The command line for a saved unit's files (Windows, Linux, the BTSpeak): list, export to a disk image, import from one, extract to a folder, pack and unpack an image. The portable half is `../../csrc/blazie/bl_files*.c` and `fat_img.c`. |
| `test_files.c` | Files in and out against the units' own commands, on all four units: the firmware's files exported exactly (the open one too), an image imported and then listed, typed into and moved by the unit, export-import-export the same image; `--break=1..5` put one bug back each for run_tests' must-fail controls. `nvda/tools/files_7zip.py` checks the images in 7-Zip. |

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

**Keys held while the Braille Lite starts.** A chord goes to the unit when its keys come up, but the keys you are
holding down are also on the unit's key port while you hold them, and the firmware looks there as it starts: i-chord
held is the cold reset ("initialize file system?", then the flash, the folders, and "delete all data in file area"),
all seven keys the warm reset, space a silent start, and so on (the Help file's list). The unit starts when it is
switched on and when p-chord, l restarts it. So: p-chord, l, then press and hold i-chord at once, until the unit asks
its first question. The firmware reads the keys about 0.45 s after l comes up (0.1 s to restart, 0.35 s into the
start): hold the chord by then. A chord the start has read is not sent again when you let go of it. Settings > Quick
key response ends at the restart, so the start keeps the unit's own pace.

**p-chord, l** (switch languages) is the Braille Lite 2000's other firmware bank: the firmware switches the program
flash's bank (port E0h bit 4), checks that a program is there, and restarts into it. The emulator holds the same
firmware in both banks, so the unit restarts in the same language, its files and settings kept.

## Settings

Settings > Sample rate (11025 to 48000 Hz; the unit restarts at the new rate with its memory kept). The unit's own
speech settings -- rate, pitch, inflection, volume -- are set on the unit, with its own keys, as on the real one.

The Braille Lite's idle channel, as Tomi's unit sounds (measured: `../../hosts/blazie_idle.py`, `tools/idle_sounds.py`;
the model: `../../csrc/blazie/bl_idle.c`). In `blazie_emu.ini`, section `[sound]`:

| Setting | Menu | Choices |
| --- | --- | --- |
| `idle` | Idle channel | `unit` (the default: the hiss at even volumes, the whine at odd, as the unit), `hiss`, `whine`, `off` |
| `keep_open` | Keep the channel open | `off` (silent as soon as speech ends: heard only under speech and 0.3 s after), `until` (the default: until the unit clicks it off, ~10 s after speech), `always` (never stops) |
| `pop_click` | The pop ... the click ... | `1` (the default): the pop when a line opens the channel after the click-off, ~0.26 s before its first phoneme, and the click at the click-off; heard with `until` only |
| `tick` | The 10 Hz tick | `1` (the default): the firmware timer's faint tick, every 100 ms while the channel is heard |

The idle noise has the same level at every unit volume, as on the unit: at volume 1 it is 29 dB under the speech, at
volume 15 50 dB under. The pop and the click are as loud at every volume too, and much louder than the speech at low
volumes (the recordings: +0.87 and -0.53 of full scale; at volume 1 the speech's loud vowels are at -29.5 dBFS). They
are the unit's line out as the line-in recorded it; on the unit's own headphones they decay faster. The screen-reader
drivers keep their own hiss and whine (no noise floor, no pop, click or tick).

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

## The clock

The time and date come from the units' clock controller, a separate chip the firmware calls over the Z180's clocked
serial port (`../../csrc/blazie/bl_clock.h` says what was measured). Before it was modelled, the firmware found no
clock: it said "reset clock, first date, then time", the time stood still and the year read 1999 (Jayson).

The first time a unit starts, its clock is set from this PC's clock. It then runs in the unit's own time, is kept in
the saved state, and goes on by the time the program was closed, as the unit's battery kept it. Set it with the
unit's own commands: on the Braille Lite o-chord, s, d (the date, m m d d y y) and o-chord, s, t (the time, h h m m,
then a or p); o-chord, t and o-chord, d read them. On the Type 'n Speak F9, s, d and F9, s, t; F4 and F5 read them.

**The year: 1989 to 2020 only.** The controller holds the year in 5 bits counted from 1989, and the 2003 firmware
sends a year as two digits plus 11: from 2021 on the number spills into the field that holds the hour. A real unit
does the same: typing 26 as the year sets the hour to 5 and leaves the year as it was. So the emulator starts the
clock in the latest year up to 2020 with the same calendar (2026 is 2015's: 1 January on a Thursday, no 29 February),
and the weekdays the unit gives are this year's. To set the date yourself, use that year too.

The alarm (o-chord, s, a) is the controller's as well: it goes off at the start of the minute set, with x for any
hour, day or month, while the unit is running.

## What the unit keeps

As a real unit keeps its battery-backed RAM, its file flash and its clock while switched off, the program saves them
on exit (and when you switch units) to `%APPDATA%\ssi263-speech\blazie-emu\english.state` or `spanish.state`, and starts from
them next time. The first time, and after Firmware > Back to the factory state, it starts from the shipped state; the
Type 'n Speak starts cold -- the program holds Ctrl+Alt+Del at power-on, the unit's own reset to its defaults
(without it blank RAM leaves the volume at 0), and the unit asks to initialise its flash: y, then y (Spanish: s).
It asks once: answered, the flash is initialised and saved with the rest, and the unit starts without the question
from then on (closed while it still asks, it asks again next time, as the unit would).

## The file flash

Both units keep their files in a 29F016-style flash chip, 2 MB as the firmware manages it (`../../csrc/blazie/flash29.c`;
the Braille Lite pages it 512 KB at a time through port E0h bits 0-1, the Type 'n Speak 128 KB through F0h). The
Type 'n Speak's firmware reads the chip's ID before it offers the flash at all. An erase takes the chip's typical time
(the Am29F016 data sheet: 32 s for the whole chip, 1 s a sector) and the firmware waits on the chip's status, clicking
through the speech chip every ~2 s meanwhile (each click: R4 F0, R1 F0, R2 FE, R3 58, phoneme 17h): initialising the
flash -- the Type 'n Speak's first start, the Braille Lite's reset -- is 32 s of clicks, then "flash initialized" or
"ready". A Braille Lite state is 768 KB (256 KB RAM + the flash's first 512 KB) while the rest of the flash is erased,
2.25 MB once files reach it; both load.

## Files in and out: disk images

Firmware > **Export files to disk image (.img)** writes every file the unit holds into a FAT disk image; Firmware >
**Import files from disk image (.img)** makes the unit's files what an image holds. No cable, no WinDisk. The image
opens in 7-Zip (and mounts on Linux: `mount -o loop`). How the units keep their files was measured on the running
firmware: `../../csrc/blazie/bl_files.h`.

**The image.** One folder for each of the unit's folders, named as the unit names them: `ram startup` and `flash
startup` (Spanish units: `RAM inicial`, `FLASH inicial`), and any you made in folder mode. Each file is under its unit
name, with its exact bytes, its time and date, and read-only if you protected it. Text keeps the unit's line ends (a
lone carriage return); a grade 2 file (on the Braille Lite: no extension, or `.brl`; on the Type 'n Speak `.brl`,
`.brf`) holds its braille as ASCII braille, like a `.brf` file, not translated to print. The help file is not exported
(its text is the firmware's). The file the unit has open is exported as it is now, with what you typed since you
opened it.

**Import** makes the unit's files what the image holds, and says what it did:
- a file whose name, folder and bytes match is left alone (so export, import, export gives the same image);
- new bytes for a file the unit has: rewritten where it is (RAM or flash), its type and protection kept;
- a new file goes into the folder it is in in the image (a flash folder: into flash; a RAM folder: into RAM); a file
  at the top of the image goes to the flash startup folder; an image folder the unit lacks becomes a new folder
  (a flash folder); folders inside folders are skipped; an empty file goes to RAM (the unit keeps none in flash);
- a file the image lacks is deleted -- except the clipboard, the datebook and the file the unit has open;
- line ends from a PC editor (CR LF or LF) become the unit's CR in text files;
- names become names the unit takes: lower case, one dot, an extension of up to 3 letters, 20 characters (the
  unit's names are one list across its folders: two files of the same name are not both imported).

The unit must not be writing its flash (the import says so and waits for you); it is switched off, its files
changed, and switched on again, so the firmware finds them as if it had written them itself.

**Editing an image.** 7-Zip opens images but cannot change them, and Windows does not open them by itself. Either
use a tool that mounts disk images (OSFMount, ImDisk), or take the image apart into a folder and put it back:

    blazie_files unpack "Braille Lite 2000 (English) files.img" myfiles
    (edit, add or delete files in myfiles\ram startup, myfiles\flash startup, ...)
    blazie_files pack myfiles changed.img

then Firmware > Import files from disk image. The same tool works on a saved unit directly (close the emulator
first: it saves its unit when it closes), for example on Linux or the BTSpeak:

    blazie_files list english.state
    blazie_files export english.state english.img
    blazie_files import english.state changed.img      (--dry-run: say what would happen)
    blazie_files extract english.state myfiles --crlf  (each file as a PC text file, CR LF)

**The Type 'n Speak's first start.** The emulator starts a new Type 'n Speak with its warm reset, which sets up the
flash but not the file system or the folders (a real unit's cold reset -- Ctrl+Alt+Del held at power-on -- asks for
all three). On such a unit the import refuses ("the unit's folders were never set up"); export works.

## Not yet

- Linux and Android shells (the portable files are ready for them; the serial port would be a tty there).
