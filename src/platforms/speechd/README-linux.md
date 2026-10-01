# The Braille Lite 2000 voice for Linux

A Blazie Braille Lite 2000 in speech-box mode, emulated: the unit's own June 2003 firmware runs on an emulated Z180
(MAME's core) and drives a register-level model of the Silicon Systems SSI-263 speech chip. The rules, number reading and inflection
are the firmware's own, live. Nothing is recorded or concatenated. English, and Spanish when its firmware is
included.

This is a speech-dispatcher module, so Orca and anything else that speaks through speech-dispatcher can use it.
It is the same voice as the NVDA add-on, byte for byte.

## Install

    tar xzf ssi263-speech-*-linux-*.tar.gz
    cd ssi263-speech-*-linux-*
    sudo ./install.sh
    killall speech-dispatcher
    spd-say -o ssi263 "Hello from the Braille Lite"

`install.sh` adds the voice and leaves your default synthesizer alone. `sudo ./install.sh --default` also makes it
the default. In Orca: Preferences, Speech, Speech synthesizer: ssi263. `sudo ./uninstall.sh` removes everything
it added.

Settings are in `ssi263.conf` beside speech-dispatcher's other module settings (the installer prints where), each
explained in the file: the sample rate (11, 22 or 44 kHz), voice inflection, the unit's hiss or whine, its tone,
and the "short pauses" line packing. For your own settings, without root and kept when you reinstall, copy any of
those lines into `~/.config/ssi263-speech/sd_ssi263.conf`: they win over the module's file. After a change,
`killall speech-dispatcher` (Orca reconnects by itself). Rate, pitch and volume come from Orca or spd-say, mapped
onto the unit's own: its factory rate 11 and pitch 16 at the middle.

Builds: x86_64 and aarch64 (a Raspberry Pi 4 or 5 is fine: the unit runs several times faster than real time on a
Pi 5). No Python, no other packages: one program.

## The Blazie emulator

`bin/blazie_emu` is the whole unit in a terminal -- a Braille Lite 2000 or a Type 'n Speak running its own firmware,
booting to its own main menu, its files and settings kept between runs -- for a Raspberry Pi's console, a desktop's
terminal or a BTSpeak. `./bin/blazie_emu` from this folder (`install.sh` also puts it in `/usr/local/bin`); F11 is
its menu. `README-blazie-emu.md` has the keys, the sound, the serial port and the BTSpeak notes.

## The firmware, and the licences

This package carries the Braille Lite's own firmware (and the Type 'n Speak's, for the emulator), shared with
permission. It is not ours; it is here so the
unit can speak again, and it will be removed if its rights holders ask.

The program and library are MIT (`LICENSE`; the chip model draws on Casso's, also MIT:
`licenses/Casso-MIT.txt`), except the Z180 CPU core, which is MAME's and keeps its BSD-3-Clause licence
(`licenses/MAME-Z180-core-BSD-3-Clause.txt`). The source and how to build it:
https://github.com/tgeczy/ssi263-speech
