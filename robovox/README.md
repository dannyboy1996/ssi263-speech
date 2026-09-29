# Robovox — SC-02 singing voice in the style of Kraftwerk's 1998 tour rig

A JUCE (9.0.3) VST3/Standalone instrument that recreates the Robovox system
from EP0396141A2 ("System for and method of synthesizing singing in real
time", Schneider / Ott / Jalass): a 6502 driving one to four Votrax SC-02
(= SSI-263) speech chips through a 6850 ACIA MIDI interface.

Start with [`RESEARCH.md`](RESEARCH.md) — the full patent/tour/hardware
analysis this build follows. The chip itself is this repository's existing
register-level model in `../src/csrc/ssi263.c`; nothing is recorded.

## Layout

| Path | What it is |
|---|---|
| `RESEARCH.md` | Patent mapping, 1998-tour deltas, SC-02 register map, emulator decisions, VST parameter table, open questions |
| `CMakeLists.txt` | JUCE 9.0.3 VST3 + Standalone build (JUCE via FetchContent) |
| `plugin/` | `RobovoxProcessor` (APVTS + audio/MIDI glue), minimal editor |
| `emu/mc6850.h` | Clean-room MC6850 ACIA model (MIDI subset: 8N1, Rx IRQ, RDRF/TDRE/OVRN) |
| `emu/robovox_bus.h` | System bus: 6502 socket, 2 KB RAM (6116 footprint), ROM socket, ACIA, 2x74LS245 SC-02 buffer, mode switch |
| `emu/robovox_firmware.h` | Clean-room translator: patent embodiment 1 (phoneme ch N, pitch ch N+1), inflection @ A=440 Hz, wheels, modes |
| `tests/` | Offline (no-JUCE) C tests: 6850 framing/IRQ, translator vectors, SSI-263 smoke render |
| `third_party/README.md` | Vendoring notes: JUCE, floooh `chips` m6502, why not MAME |

## Phase 1 status (this commit)

- MIDI → SC-02 register translation runs in C, per the patent's first
  embodiment, through the emulated 6850's real status/RDRF/IRQ path.
- It is a **two-track instrument**: the phoneme channel alone sings at
  ~30 Hz (inflection 0) — put pitch notes on channel N+1 (or a second
  clip) or you will hear almost nothing on small speakers. That is the
  patented method, not a bug.
- The key→phoneme layout is fixed and built in (`note_map.txt` documents
  it — reference only, the plugin reads no files). A custom layout becomes
  a UI feature (Phase-2 PEC-style editor), not a sidecar file.
- The 6502 socket, memory map, IRQ line and ROM image slot exist and are
  exercised by the tests; the 6502 core itself (floooh `chips` `m6502.h`,
  MIT) drops in via `third_party/` without touching the translator API.
  Moving the translator into a 6502-resident ROM image is the explicit
  Phase-2 milestone — the bus is ready for it.
- Single-chip (SEQ) and quad-chip (tour rig, channels 1/3/5/7 + 2/4/6/8)
  configurations. External-carrier input = second audio input pair.

## Build

Tests first (no network needed):

```
cmake -S robovox/tests -B build/robovox-tests
cmake --build build/robovox-tests --config Release
ctest --test-dir build/robovox-tests -C Release
```

VST3 (needs network once for JUCE 9.0.3 + a C++20 toolchain):

```
cmake -S robovox -B build/robovox -DCMAKE_BUILD_TYPE=Release
cmake --build build/robovox --target Robovox_VST3 --config Release
```

Artifacts land in `build/robovox/Robovox_artefacts/Release/`:
`VST3/Robovox.vst3` (the plugin bundle — copy the whole `Robovox.vst3`
folder to `C:\Program Files\Common Files\VST3`) and
`Standalone/Robovox.exe` (play it without a DAW). Full instructions,
prerequisites, and the "ways to build it wrong" table are in
[`BUILD.md`](BUILD.md). Verified 2026-09-29
with JUCE 9.0.3 (latest): VST3 links, `GetPluginFactory` exported,
`moduleinfo.json` lists Robovox as Instrument/Synth/Vocal, offline C
tests pass, and a sung "hello" renders end-to-end through the
MIDI → 6850 → translator → SC-02 path (`robovox_hello.wav` in this
directory — H-E-L-O on ch 1, E4→G4 on ch 2).

## Licenses

Our code (including the clean-room 6850 and translator): MIT, like the
rest of this repository. JUCE (AGPLv3/commercial), floooh `chips` (MIT)
keep their own terms — see `third_party/README.md`. The Robovox patent
EP0396141A2 is withdrawn; the original Robovox firmware/Atari software is
lost, so the translator here is a new implementation of the patent text.
