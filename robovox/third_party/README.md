# Vendored / fetched components

## JUCE 9.0.3 — fetched at configure time, not vendored

`CMakeLists.txt` pulls `juce-framework/JUCE` tag `9.0.3` (published
2026-09-28, the current latest) via FetchContent. JUCE is AGPLv3 /
commercial dual-licensed; nothing of JUCE is copied into this tree.

## floooh `chips` `m6502.h` — drop-in, MIT

The 6502 core is **not** vendored here to keep the diff small. To enable
real 6502 execution on the emulated bus:

```
git clone --depth 1 https://github.com/floooh/chips.git third_party/chips
```

then configure with `-DROBOVOX_HAVE_M6502=ON`. Only `chips/m6502.h`
(header-only, MIT, cycle-steppable via pin-callback interface) is used.
`#include "chips/m6502.h"` must resolve — e.g. `-I third_party/chips`.

`floooh/chips` has no MC6850, hence the clean-room `emu/mc6850.h`.

## Why not MAME's 6502 / 6850

MAME's `src/devices/cpu/m6502/m6502.cpp` (Olivier Galibert) and
`src/devices/machine/6850acia.cpp` are per-file BSD-3-Clause, but both are
written against MAME's internal device framework (`device_t`, address
spaces, scheduler, save states). Extracting them means porting the
framework or rewriting the cores — at which point they are no longer
"MAME's" cores. MAME as a project is GPL-2.0+, which would also taint a
VST binary. The header-only MIT 6502 plus a ~200-line purpose-built 6850
(MIDI needs 8N1 + Rx IRQ + two status bits) is smaller, auditable, and
license-clean. The 6850's observable MIDI behavior is covered by
`tests/test_6850_midi.c`.
