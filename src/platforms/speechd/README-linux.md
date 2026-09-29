# The Braille Lite 2000 voice for Linux

A Blazie Braille Lite 2000 in speech-box mode, emulated: the unit's own June 2003 firmware runs in z180emu and
drives a register-level model of the Silicon Systems SSI-263 speech chip. The rules, number reading and inflection
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

Builds: x86_64 and aarch64 (a Raspberry Pi 4 or 5 is fine: the unit runs about ten times faster than real time on
a Pi 5). No Python, no other packages: one program.

## The firmware, and the source

This package carries the Braille Lite's own firmware, shared with permission. It is not ours; it is here so the
unit can speak again, and it will be removed if its rights holders ask.

z180emu is GPLv2 (`COPYING.z180emu`); the complete source of these binaries, and how to build them, is in
`source/`. The rest of the project is MIT (`LICENSE`): https://github.com/tgeczy/ssi263-speech
