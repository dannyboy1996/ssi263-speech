# The Blazie emulator on Linux

A Braille Lite 2000 or a Type 'n Speak in a terminal: Blazie's own firmware on the emulated board, with the emulated
SSI-263 as its voice -- the same unit as the Windows app (`emu_unit.c`), on a Raspberry Pi's console, a desktop's
terminal, or a BTSpeak, Blazie Technologies' Linux notetaker on ARM: the old Blazie units running on the new one.

It boots to the unit's own main menu and behaves as the unit does. What you type goes to the unit as its keys; what
the program itself says (its menu, its messages) is plain lines of text, which the console's screen reader reads
(Speakup, Orca, BRLTTY's speech on the BTSpeak).

## Build

    sudo apt install build-essential pkg-config libasound2-dev
    ./build_linux.sh

`build/linux/blazie_emu`: one program with the C++ runtime inside; it needs only the C library, the maths library and
ALSA's `libasound` (on every Linux with sound). It is built with MAME's Z180 core (BSD-3-Clause) and the project's
own MIT code only -- never z180emu -- so it is MIT, with MAME's notice for the Z180 core; `tools/check_no_gpl.py`
checks the program and the package. Without ALSA's headers the build uses PulseAudio's simple API when `libpulse-dev`
is there (or ask for it: `BLAZIE_AUDIO=pulse ./build_linux.sh`); with neither, the emulator is not built and
`tools/linux_tests.sh` fails saying so.

The Linux package (`tools/package_linux.sh`) carries it as `bin/blazie_emu` with this file as
`README-blazie-emu.md`, the firmware in `share/ssi263-speech` (the Type 'n Speak's in its `tns` folder), and the
licences (`LICENSE`, `licenses/MAME-Z180-core-BSD-3-Clause.txt`, `licenses/Casso-MIT.txt`). Run it from the unpacked
package (`./bin/blazie_emu`), or `sudo ./install.sh`, which also puts it in `/usr/local/bin` with its firmware.

**Which Linux.** A program built on one distribution runs on that one and newer: the package's 0.7 builds come from
Debian 13 (glibc 2.38 or later). On an older system -- quite possibly a BTSpeak, whose system is Raspberry Pi OS of
some version -- build it on the machine itself (the three commands above), which also covers a 32-bit (armhf) system.

## Run

    blazie_emu                    the unit you used last (the first time: the English Braille Lite)
    blazie_emu --unit tns-en      bl-en, bl-es (the Spanish Braille Lite), tns-en, tns-es (the Type 'n Speak)
    blazie_emu --show-keys        what this keyboard sends: the terminal's bytes and the keys they are, and the
                                  input devices' keys going down and up (for the key settings; q q stops it)
    blazie_emu --no-sound         no sound card: the unit runs on silent, paced by the system clock
    blazie_emu --help

The firmware is looked for in `firmware_dir` in the settings, else beside the program: the package's
`../share/ssi263-speech`, a `firmware` folder, or the source tree's `firmware/blazie` (run from `build/linux`); or
give it: `--firmware DIR`. Either layout works: the repository's (`BL2ENG.BNS`, `spanish/`, `tns/`) or all in one
folder.

**F11 opens the menu** (and Ctrl+O for the Braille Lite; Alt+Shift+F always, as on Windows). Type a number and
Enter; Enter alone goes back to the unit:

| | |
| --- | --- |
| 1-4 | The unit: Braille Lite 2000 English or Spanish, Type 'n Speak English or Spanish |
| 5 | Back to the factory state (erases this unit's files; type yes) |
| 6 | Sample rate, 11025-48000 Hz (the unit restarts at it, its memory kept) |
| 7-10 | The Braille Lite's idle channel: the sound (as the unit, hiss, whine, silent), keeping it open (off, until the unit clicks off, always), the pop and click, the 10 Hz tick -- as on Windows (README.md) |
| 11 | Quick key response (faster than the real unit) |
| 12 | The serial port (below) |
| 13 | The Braille Lite's keyboard: keys or letters (below) |
| 14 | The keys, in short |
| 0 | Exit: the unit's memory is saved |

After a choice the menu says one line; `?` lists it again.

## The settings and the units' memory

`~/.config/ssi263-speech/blazie-emu/` (or under `$XDG_CONFIG_HOME`; `--config DIR` for another):
`blazie_emu.ini`, written the first time with every key setting explained, and each unit's memory --
`english.state`, `spanish.state`, `tns_english.state`, `tns_spanish.state` -- saved when you exit, when you switch
units, and every minute (written whole, then put in place: a power cut never leaves half a file). The first time,
the Braille Lite starts from the shipped state and the Type 'n Speak as a new unit: its own cold reset (Ctrl+Alt+Del
held at power-on) asks how to set itself up, and the program says so. Press y for each question (the Spanish unit:
s), seven times: initialize file system, are you sure; initialize flash system, are you sure (then the flash chip's
~46 seconds of clicks); initialize folder system (it says it is ready and opens its help); delete all data in file
area, are you sure (then about 35 seconds of silence while it clears its memory, and it starts again). README.md,
"The Type 'n Speak's first start", says why each one matters.

A Type 'n Speak saved by the 0.6 or 0.7 previews was never set up (their first start missed the unit's cold reset:
a new file lost its first letter, a file moved to flash was lost). When such a unit starts, the program says so and
asks: `k` sets it up now and keeps its RAM files (its settings go back to the factory's), `f` starts it from the
factory state (its own questions again), Enter alone starts it as it is. `k` and `f` keep the old memory beside it
as `tns_english.state.before-setup`.

`blazie_files` (in the package's `bin`, beside `blazie_emu`) lists, exports, imports, extracts, packs and unpacks a
saved unit's files from the command line, as on Windows (README.md, "Files in and out"); close the emulator first.

## Keys

### Braille Lite: three ways in

A terminal says when a key goes down, never when it comes up, and BRLTTY (the BTSpeak's keyboard driver) hands a
program characters, not keys. So the program takes the Braille Lite's six keys, space bar and advance bar three ways:

**Keys mode** (`[keys] mode = keys`, the default; a PC keyboard in a terminal): F D S = dots 1 2 3, J K L = dots
4 5 6, the space bar, A or ; = the advance bar. The keys you type close together are one chord: it goes to the unit
80 ms after the last (`chord_ms`). Holding a chord down does not repeat it (a key typed again within 150 ms,
`repeat_ms`, is the keyboard's auto-repeat). The keys are set in `[keys]` (`dot1 = f brl_dot1` ...).

**Letters mode** (`[keys] mode = letters`, or menu 13; the BTSpeak's own braille keyboard, or any braille display's
keyboard through BRLTTY): each character typed is its braille cell, in computer braille (North American Braille
Computer Code, which BRLTTY's US tables type): `p` is dots 1 2 3 4, `4` is dots 2 5 6, space is the space bar.
Each chord goes to the unit at once. Chords with the space bar, which BRLTTY keeps for itself:

- the chord prefix, **Ctrl+C**, then the character: Ctrl+C p is p-chord (`[letters] prefix`);
- a **capital letter** -- dot 7 with the letter on a BTSpeak -- is the letter's chord: `P` is p-chord
  (`capital_is_chord = 1`);
- the keys BRLTTY makes of chords, mapped back to them, as the BTSpeak's own table (its user's manual, "Basic
  Keyboard Navigation"): Up = dot-1 chord, Down = 4, Left = 3, Right = 6, Ctrl+Left = 2, Ctrl+Right = 5,
  Page Up = 2-3, Page Down = 5-6, Home = 1-3, End = 4-6, Ctrl+Home = 1-2-3, Ctrl+End = 4-5-6, Tab = 4-5,
  Shift+Tab = 1-2, Insert = 3-5, Delete = 2-5-6, Esc = 2-6 chord; and Enter (dot 8) = e-chord, Backspace (dot 7) =
  b-chord, the unit's own Enter and Backspace; Ctrl+A = the advance bar. Each is a line in `[letters]`, key = chord:
  `up = 1-chord`, `f9 = dots 1 3`, `ctrl-a = advance`, `delete = none`.

**Input devices** (`[input] evdev`): Linux's `/dev/input` devices report each key going down and coming up, so the
chords work as on Windows -- the chord when the last key comes up, and the keys held down seen as held. `auto` (the
default) uses them on a text console, when a keyboard can be read; `on` everywhere (on a desktop, mind that they are
read whichever window is in front); or a device's path. Reading them needs the `input` group: `sudo usermod -aG input
$USER`, then log in again. With `grab = 1` (the default) only this program gets those keys while it runs -- not the
console, not a screen reader -- except while its menu is open; the kernel lets go if the program ends. A braille
keyboard that Linux reports with its own dot keys (`brl_dot1`..`brl_dot8`) is mapped too. A keyboard another
program holds for itself (BRLTTY can) is left alone and named at the start ("held by another program"): its keys
come through the terminal as characters, so letters mode is the way in there. With the devices grabbed, keys that
still reach the terminal can only be another keyboard's, and go to the unit too.

### Keys held: the hold key

The Braille Lite reads the keys held down while it starts -- when it is switched on, and when p-chord, l restarts
it: i-chord held is the cold reset ("initialize file system?"), all seven keys the warm reset (README.md, "Keys held
while the Braille Lite starts"). The input devices see keys held as they are. From a terminal, the **hold key**
(F12 or Ctrl+K, `[keys] hold`) does it: press it, and the next chord typed is held down instead of sent, until you
press it again -- then it comes up and is sent, as the keys coming up would be. A chord typed meanwhile is sent as
usual. The cold reset: p-chord, hold key, i-chord, l -- the unit restarts with i-chord held and asks "initialize
file system?" -- then the hold key again. The program says "Holding i-chord" and "Let go".

### Type 'n Speak

The whole keyboard is the unit's (its key codes: `tns_keys.h`, the same table as Windows'). From a terminal each
key is pressed whole -- the modifiers it needs down, the key down and up, the modifiers up: `A` is Shift and a,
Ctrl+O is Ctrl and o, Alt+X is Alt and x, `!` is Shift and 1 (a US keyboard's shifted characters). From the input
devices each key goes down and up as you move it. F11 or Alt+Shift+F opens the menu (`[keys] tns_menu`).

## Sound

ALSA's `default` device (`[sound] device` in the settings for another: `hw:0`, `plughw:1`, ...), 16-bit mono at the
unit's rate (44100 by default), in blocks of 10 ms, four deep (`[sound] block_ms`, 5-20) -- as the Windows app, a
key's speech plays behind 30-40 ms of queued sound. A thread renders each block as the card takes it, so the card's
clock paces the unit; it asks for real-time priority and runs without it. On a desktop, ALSA's default device
reaches PulseAudio or PipeWire through their ALSA plugin. With no sound card (or `--no-sound`) the unit runs on,
silent, paced by the system clock.

On a Raspberry Pi 5 the running unit takes about a fifth of one core at 44100 Hz (measured with ALSA, PulseAudio
and silent). A BTSpeak's Compute Module 4 is two to three times slower: if the sound breaks up there, choose 22050
Hz (menu 6), or `[sound] block_ms = 20`.

Saving the unit's memory (every minute, and on exit) holds the unit while the file is written, as on Windows: the
Type 'n Speak's is 5 MB, so on slow storage a short gap in the sound can be heard once a minute.

## The serial port

Menu 12: a serial device (`/dev/ttyUSB0`, `/dev/ttyACM0`, `/dev/ttyS0`, `/dev/ttyAMA0`; the menu lists the ones
present; the `dialout` group is needed: `sudo usermod -aG dialout $USER`) or `pty`, a pseudo-terminal whose other
end a program on the same machine opens: the program says its name and links it as
`~/.config/ssi263-speech/blazie-emu/serial`. A terminal program (`picocom`, `minicom`), or a DOS disk tool under
DOSBox with its serial port on that device, then talks to the unit. The line follows the firmware as on Windows
(`serial_win.c`; what the firmware does: `../../csrc/blazie/bl_serial.h`): the rate, data bits and parity it
programs, set in step with its bytes (the storage commands switch to 19200 and back), DTR while its port is on, RTS
for its handshake line, its XON and XOFF passed through as bytes (the tty's own flow control is off). The choice is
kept in `[serial] port` and used again next time; a port missing then leaves the unit unplugged, the setting kept.

## Tests

`tools/linux_tests.sh` runs, on the Linux machine (no sound card, no firmware in the repository: a firmware folder
is given):

- `test_keys` -- the keyboard without a unit: a terminal's sequences (xterm, VTE, the Linux console), keys mode's
  chords by time and its auto-repeat, the hold key, letters mode's computer braille and mapped keys, an input
  device's keys down and up, the Type 'n Speak's strokes, the settings file. Its control (`BLAZIE_KEYS_BREAK=1`,
  dots 1 and 4 swapped) must fail its Braille Lite checks and only those.
- `test_emu_unit`, `test_clock` -- the unit headless as on Windows, here on MAME's Z180: the boot, a chord answered,
  key latency, saving; the clock controller, the date and time set and read with the units' own commands, i-chord
  held through a restart (its control drops the held keys and must fail).
- `test_rescue` -- a Type 'n Speak made as the previews made it (no file system, no folders; a file moved to flash
  lost) is told apart and set up anew with its RAM files; its control leaves the old cold start on and must fail.
- `tools/blazie_files_roundtrip.sh` -- `blazie_files` on a copy of the shipped state: export, unpack, a new file,
  pack, import; the unit lists it and every old file is unchanged. Its control (`TEST_FILES_BREAK=2`, a new flash
  file's blocks left unmarked) must fail.
- `test_emu_linux.py` -- the whole program headless (`--null`, keys typed from a script at their times through the
  terminal's decoding): the Braille Lite boots and answers F (not without it); o-chord t typed as keys and as letters
  says the host's time; the Type 'n Speak from cold, its seven setup questions answered y, F4 says the time; the date
  and time set through the unit's
  commands, saved to the memory folder and started from again; i-chord held through a restart by an input device's
  keys; and the program run as a person runs it, in a pseudo-terminal, its serial port on another: s-chord's XON ENQ
  at 19200 bit/s, ACK answered with 'C', NAK (the control) not. Its control (`BLAZIE_KEYS_BREAK=1`) must fail the
  two clock checks.

Also run by hand on a Raspberry Pi 5 (Debian 13, arm64): the input devices with a virtual keyboard (uinput) --
found, grabbed, a chord down and up, F11 opening the menu; and the same keyboard held by another program: named,
the terminal's keys used --; the menu itself in a terminal; the sound with ALSA and with the PulseAudio build,
paced by the device (about a fifth of a core); each unit started from the unpacked package.

## Not yet verified on a real BTSpeak

None of this has run on a BTSpeak yet. To find out there:

- which build runs (its system's glibc; 64- or 32-bit) -- else build it on the device;
- what its braille keyboard sends in a terminal: `blazie_emu --show-keys`, in the Blazie-mode console and in Desktop
  mode's terminal. Letters mode assumes US computer braille with dot 7 for capitals, and BTSpeak's desktop table for
  the chords it turns into keys; whether Ctrl+C, Ctrl+K and Ctrl+O (or F11 and F12) can be typed on it at all, and
  which chords never reach a terminal, is unknown -- `[letters]` and `[keys]` remap them;
- whether its braille keyboard is an input device the program may read (`--show-keys` lists the devices and their
  keys): if BRLTTY holds it, only letters mode works; if it can be read, the six keys and dots 7 and 8 work as keys
  with keys held, which is the best way in;
- its sound: ALSA's default device there, and whether the unit's voice and the screen reader's share the speaker;
- the Type 'n Speak needs a QWERTY keyboard (a USB one on the BTSpeak's USB-C port).

Not tried on any machine: WinDisk or PCDISK on the far end of the serial port (the tests answer the unit's storage
call themselves), a real serial adapter, a real keyboard on the input devices (a virtual one was), sound actually
heard (the Pi used had no speaker: the devices took the sound at the right pace), an x86-64 build (the 0.7 work
was tested on arm64 only).
