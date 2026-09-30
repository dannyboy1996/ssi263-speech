# Third-party code

Kept apart from this project's own, each with its licence, its upstream and what we changed.

| Folder | What | Licence | Used for |
| --- | --- | --- | --- |
| `z180emu/` | the Z180 core of z180emu (a C port of MAME's Z180), with three patches (`PATCHES.md`) | GPL-2.0-or-later | the Braille Lite's CPU on the legacy path (`src/csrc/cpu/z180_legacy.c`) |
| `casso/` | Casso's SSI-263 model and ROM-extraction notes (`UPSTREAM.json`) | see its `LICENSE` | reference only |

MAME's Z180 and 8085 are not here: they are extracted into generated files under `src/csrc/cpu/` from a pinned
MAME revision (`extract_*_machine.py`, BSD-3-Clause notices kept).
