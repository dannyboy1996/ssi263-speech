# Building Robovox

## You need

- Windows 10/11 with **Visual Studio 2022 or newer** ("Desktop development
  with C++" workload — the build uses MSVC).
- **CMake 3.22+** (`cmake --version`) and **git** (`git --version`).
- Internet **once**: JUCE 9.0.3 is fetched automatically from GitHub
  (~200 MB into `build/_deps`, reused afterwards). Everything else
  (the SSI-263 engine, the 6502/6850 emulator, the translator) is
  already in this repo — no submodules, no SDKs.

Check: `cmake --version`, `git --version`, and that `cl.exe` exists
(the "x64 Native Tools Command Prompt", or plain PowerShell if the
VS installer put the compiler on PATH).

## Build (PowerShell, from the repo root)

```
cmake -S robovox -B build/robovox
cmake --build build/robovox --target Robovox_VST3 --config Release
```

`--config Release` is mandatory: MSVC is multi-config and the default
is a Debug build (works, but slow, with assert popups).

Other targets from the same tree:

```
cmake --build build/robovox --target Robovox_Standalone --config Release
cmake --build build/robovox --target test_6850_midi --config Release
ctest --test-dir build/robovox -C Release
```

The offline C tests (`test_6850_midi`: 6850 framing/IRQ, patent MIDI
vectors, bus path, engine smoke, envelope, VST render order) need no
JUCE and also configure standalone:
`cmake -S robovox/tests -B build/rb-tests` + same build/test lines.

## Install

- **VST3**: copy the whole folder
  `build/robovox/Robovox_artefacts/Release/VST3/Robovox.vst3`
  to `C:\Program Files\Common Files\VST3\`, then rescan plug-ins in
  your DAW. (Ableton: Preferences → Plug-ins → Rescan. If an old build
  persists, remove the folder, rescan once with it absent, then
  re-add — DAWs cache VST3s.)
- **Standalone**: run
  `build/robovox/Robovox_artefacts/Release/Standalone/Robovox.exe`
  (Options → Audio/MIDI settings: enable a MIDI input).

## Ways to build it wrong

| Symptom | Cause | Fix |
|---|---|---|
| No `.vst3` anywhere, only a folder with `Contents/x86_64-win/` empty or a `.lib` | Built target `Robovox` (shared-code library) instead of `Robovox_VST3` | Build `--target Robovox_VST3` |
| Plugin loads but audio stutters / asserts | Debug build (forgot `--config Release`) | Rebuild with `--config Release`, reinstall |
| New build, old behavior | DAW cached the previous `.vst3` | Remove, rescan empty, re-add, rescan |
| `ctest` reports "Not Run" | Test binary not built, or `-C` missing | Build `--target test_6850_midi`, run `ctest -C Release` |
| `LNK1104 cannot open .obj` / `LNK1136 corrupt file` mid-build | Stale `cl`/`MSBuild` processes from an interrupted build holding locks (then killed mid-compile) | Kill strays (`Get-Process cl,msbuild`), delete `build/robovox/Robovox.dir`, rebuild. Build one `--target` per invocation. |
| Configure re-downloads JUCE every time | Deleted `build/` | Keep `build/` (git-ignored); reconfigure reuses `build/_deps` |

## First sound (two-track method, per the patent)

1. Phoneme clip on **MIDI channel 1**, pitch clip on **channel 2**
   (defaults; "Phoneme channel" param moves the pair).
2. Channel 1 alone sings at ~30 Hz — near-inaudible without channel 2.
   That is the patented method, not a bug.
3. Key→phoneme layout: `note_map.txt` (reference; C4 = middle C).
4. No audio checklist: MIDI reaching the track (armed/monitored?) →
   channels 1+2 → velocity > 0 → Volume param up → pitch notes present.

## Layout reference

`CMakeLists.txt` (JUCE 9.0.3 via FetchContent, VST3 + Standalone),
`plugin/` (processor), `emu/` (`mc6850.h`, `robovox_bus.h`,
`robovox_firmware.h`), `tests/`, `note_map.txt`, `robovox_hello.wav`
(demo render), `RESEARCH.md` (patent/tour/hardware analysis).
