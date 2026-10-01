# Building SSInger

One CMake project builds every format on every desktop OS:

| Format | Windows | macOS | Linux | Hosts it is for |
|---|---|---|---|---|
| VST3 | yes | yes | yes | REAPER, Cubase, Studio One, Ableton Live, Bitwig, FL Studio, Ardour, ... |
| CLAP | yes | yes | yes | Bitwig, REAPER, FL Studio, MultitrackStudio, ... |
| AU (AudioUnit v2) | - | yes | - | Logic, GarageBand, MainStage (and most other Mac hosts) |
| LV2 | (opt-in) | (opt-in) | yes | Ardour, Carla, Qtractor, Zrythm, ... |
| Standalone | yes | yes | yes | no DAW: play it from a MIDI keyboard |
| AAX | no | no | - | Pro Tools: see "Not built: AAX" below |

macOS builds are universal (Apple silicon arm64 + Intel x86_64). Linux is
built and tested on x86-64 and arm64 (a Raspberry Pi 5 and GitHub's arm64
runners). The workflow `.github/workflows/ssinger.yml` builds all of this on
Windows, Linux x86-64, Linux arm64 and macOS, runs the tests and the
validators below, and keeps the bundles as downloadable artifacts.

## What the built plug-ins run on

| OS | Runs on | Why |
|---|---|---|
| Windows | **Windows 10 (version 1607) or later**, x64. Not Windows 7 or 8.1: the binaries will not load there. | JUCE 8 and 9 support Windows 10 1607+ only; JUCE imports dcomp.dll, the shcore scaling API and user32's per-monitor DPI functions directly. JUCE 9 has no supported switch to lower this. |
| macOS | **macOS 10.15 (Catalina) or later**, Apple silicon or Intel (one universal binary) | Built with `CMAKE_OSX_DEPLOYMENT_TARGET=10.15` |
| Linux | x86-64 or arm64 with **glibc 2.35 and libstdc++ from GCC 12 or newer**: Ubuntu 22.04+, Debian 12+, Fedora 36+, Raspberry Pi OS Bookworm or later. Older systems (Debian 11, Ubuntu 20.04): build from source. | The downloads are built on GitHub's Ubuntu 22.04 runners; their binaries ask for GLIBC_2.35, GLIBCXX_3.4.30 and CXXABI_1.3.13 (checked on this build) |

## You need

Everywhere: **CMake 3.22+** and **git**, and internet **once**: JUCE 9.0.3
and clap-juce-extensions are fetched from GitHub at configure time
(~250 MB into `build/<dir>/_deps`, reused afterwards). Everything else (the
SSI-263 engine, the 6502/6850 emulator, the translator) is already in this
repo.

- **Windows 10/11** (the plug-ins run on Windows 10 1607 or later only,
  never Windows 7 or 8.1): Visual Studio 2022 or newer, or its Build Tools, with
  the "Desktop development with C++" workload (MSVC). The plug-ins link the
  C runtime statically, so users need no Visual C++ redistributable.
- **macOS 10.15+** to run, **Xcode 12.4+** (Apple clang) to build. Ninja
  (`brew install ninja`) is optional but faster than Makefiles.
- **Linux**: GCC (g++) or Clang, and JUCE's development packages. On
  Debian, Ubuntu or Raspberry Pi OS:

  ```
  sudo apt install g++ cmake ninja-build pkg-config \
      libasound2-dev libjack-jackd2-dev ladspa-sdk \
      libfreetype-dev libfontconfig1-dev \
      libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
      libxrandr-dev libxrender-dev libxi-dev \
      libglu1-mesa-dev mesa-common-dev libegl-dev
  ```

  (No WebKit or curl: the plug-in turns JUCE's web browser and curl off.)
  Fedora names: `alsa-lib-devel jack-audio-connection-kit-devel
  ladspa-devel freetype-devel fontconfig-devel libX11-devel
  libXcomposite-devel libXcursor-devel libXext-devel libXinerama-devel
  libXrandr-devel libXrender-devel libXi-devel mesa-libGLU-devel
  mesa-libEGL-devel`.

## Build (from the repo root)

**Windows** (PowerShell or any prompt; MSVC is multi-config, so
`--config Release` is mandatory: the default is a slow Debug build with
assert pop-ups):

```
cmake -S SSInger -B build/SSInger
cmake --build build/SSInger --config Release --target SSInger_VST3 SSInger_CLAP SSInger_Standalone
```

`SSInger\build_vst.bat` does the VST3 build and the tests in one go.

**macOS** (universal):

```
cmake -S SSInger -B build/SSInger -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15
cmake --build build/SSInger --target SSInger_VST3 SSInger_AU SSInger_CLAP SSInger_Standalone
```

**Linux**:

```
cmake -S SSInger -B build/SSInger -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/SSInger --target SSInger_VST3 SSInger_CLAP SSInger_LV2 SSInger_Standalone
```

A Raspberry Pi 5 (4 cores, 8 GB) builds every format from clean in about
6.5 minutes (measured, not counting the one-time download);
`--parallel 2` if a smaller board runs out of memory.

Options (`-D...=ON/OFF` at configure time):

| Option | Default | What it does |
|---|---|---|
| `SSINGER_CLAP` | ON | Build the CLAP too (fetches clap-juce-extensions) |
| `SSINGER_LV2` | ON on Linux, OFF elsewhere | Build the LV2 too |
| `SSINGER_COPY_AFTER_BUILD` | OFF | Copy each plug-in into your user plug-in folder after building |
| `SSINGER_BUILD_TESTS` | ON | The offline C tests |

Artifacts land in `build/SSInger/SSInger_artefacts/Release/`:

| Format | Windows | macOS | Linux |
|---|---|---|---|
| VST3 | `VST3/SSInger.vst3` (folder) | `VST3/SSInger.vst3` | `VST3/SSInger.vst3` |
| CLAP | `CLAP/SSInger.clap` (file) | `CLAP/SSInger.clap` (bundle) | `CLAP/SSInger.clap` (file) |
| AU | - | `AU/SSInger.component` | - |
| LV2 | - | - | `LV2/SSInger.lv2` (folder) |
| Standalone | `Standalone/SSInger.exe` | `Standalone/SSInger.app` | `Standalone/SSInger` |

## Tests

The offline C tests need no JUCE and also configure on their own:

```
cmake -S SSInger/tests -B build/SSInger-tests
cmake --build build/SSInger-tests --config Release
ctest --test-dir build/SSInger-tests -C Release --output-on-failure
```

They cover the 6850 (framing, IRQ), the patent's MIDI vectors, the bus
path, the engine, latches, tuning, mono priority, and the host conditions
a DAW creates: the same song renders sample-for-sample the same at any
block size (1 to 8192), at 44.1, 48 and 96 kHz the chip keeps its own
time and level, and with no notes the output stays at the chip's idle
floor (under -70 dBFS).

The plug-ins themselves are checked with the format validators, as in CI:

```
pluginval --strictness-level 10 --validate <path to SSInger.vst3>
pluginval --strictness-level 10 --validate ~/Library/Audio/Plug-Ins/Components/SSInger.component   (macOS)
auval -strict -v aumu Ssng S263                                                                (macOS)
clap-validator validate <path to SSInger.clap>
LV2_PATH=<.../LV2> lv2info urn:ssi263-speech:ssinger                                           (Linux, lilv-utils)
```

pluginval (Tracktion, GPL-3.0) loads the plug-in the way a host does and
checks state save/restore, sample rates 44.1/48/96 kHz, block sizes 64 to
1024, parameter fuzzing, editor open/close, bus layouts and threading. On
Linux run it under `xvfb-run -a` if there is no display. pluginval ships
no arm64 Linux binary; CI builds the same tag from source there.

## Install

Copy the bundle (the whole folder or file) to one of these folders, then
rescan plug-ins in the DAW.

| Format | Windows | macOS | Linux |
|---|---|---|---|
| VST3 | `%COMMONPROGRAMFILES%\VST3` | `~/Library/Audio/Plug-Ins/VST3` (or `/Library/Audio/Plug-Ins/VST3` for all users) | `~/.vst3` (or `/usr/local/lib/vst3`, `/usr/lib/vst3`) |
| CLAP | `%COMMONPROGRAMFILES%\CLAP` (or `%LOCALAPPDATA%\Programs\Common\CLAP`) | `~/Library/Audio/Plug-Ins/CLAP` | `~/.clap` (or `/usr/lib/clap`) |
| AU | - | `~/Library/Audio/Plug-Ins/Components` | - |
| LV2 | `%APPDATA%\LV2` (if built) | `~/Library/Audio/Plug-Ins/LV2` (if built) | `~/.lv2` (or `/usr/local/lib/lv2`, `/usr/lib/lv2`) |
| Standalone | anywhere | `/Applications` or anywhere | anywhere |

`%COMMONPROGRAMFILES%` is the "Common Files" folder inside Program Files;
copying there needs an administrator prompt.

### macOS: Gatekeeper and unsigned plug-ins

CI signs every macOS bundle **ad hoc** (`codesign --sign -`): enough for
Apple silicon to load the code and for auval and pluginval to pass, but it
is not an Apple Developer ID signature and the bundles are not notarized.
A copy downloaded with a browser carries the quarantine flag, and macOS
then refuses it ("is damaged" or "cannot be opened") or the host silently
skips it. After copying, clear the flag once per bundle:

```
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/SSInger.vst3
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/SSInger.component
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/SSInger.clap
xattr -dr com.apple.quarantine /Applications/SSInger.app
```

A bundle you built yourself is not quarantined. Logic and GarageBand only
see a new AU after the AudioUnit cache refreshes: log out and in, or run
`killall -9 AudioComponentRegistrar`, then Logic's Plug-in Manager "Reset &
rescan selection".

Signing with a Developer ID and notarizing (so downloads open with no
`xattr` step) needs a paid Apple Developer account: that is Tomi's
decision, not a build step yet.

### Not built: AAX (Pro Tools)

Compiling an AAX is not the obstacle: JUCE 9.0.3 ships Avid's AAX SDK,
under Avid's developer agreement or, alternatively, GPL v3. Loading it is:
release builds of Pro Tools load only AAX binaries signed with PACE's
signing tools. Avid gives those out free, but only after signing up as an
AAX developer, testing in the special Pro Tools Developer build, and
sending Avid a request with a recording of the plug-in running there; then
PACE signs you up (with an iLok). That is out of reach for this project
for now, so there is no AAX target. Pro Tools users can host the VST3 or
AU through a wrapper plug-in (for example Blue Cat's PatchWork).

## Using it in a DAW without seeing the screen

Every control is a host-automatable parameter with a plain name and value
text, so the DAW's own generic parameter view is a complete,
screen-reader-friendly way to play SSInger, on every OS:

| Parameter | Values (as the host shows them) |
|---|---|
| Phoneme channel | 1 to 16 (pitch notes go on the next channel up) |
| Voices | SEQ (1 voice), Quad (tour rig) |
| MIDI embodiment | N/N+1 phoneme+pitch, Expander (PC) |
| Wheel map | Patent (bend=filter), Clock bend (Polaxis-style) |
| Articulation (ART) | 0 to 7 |
| Filter frequency (FF) | 0 to 255 (with four chips: chip 1's, and the base the others follow) |
| Chip 2 filter offset (tour rig) | "0 (same as chip 1)", "+12 from chip 1", "-30 from chip 1" (-255 to +255) |
| Chip 3 filter offset (tour rig) | as above |
| Chip 4 filter offset (tour rig) | as above |
| Rate | 0 to 15 |
| Pitch glide (R1) | 0 to 7 |
| Phoneme DUR | 0 to 3 |
| Velocity curve | Linear, Log (:L:) |
| Bend range (st) | "24.0 st" (1 to 48) |
| Master clock (st) | "+0.0 st (1.000 MHz)" (-24 to +24) |
| Carrier | Internal, External (sidechain) |
| Volume | "-1.9 dB" (-inf to +3.5 dB) |

The three "Chip N filter offset (tour rig)" parameters only do something
when Voices is "Quad (tour rig)"; with one voice they are stored and have
no effect. In the tour rig the mod wheel (CC1, with the default Clock bend
wheel map; the pitch wheel with the Patent map) moves all four chips'
filters at once, whichever of channels 1-8 it comes on: chip 1 goes to the
wheel's value and chips 2-4 to that value plus their offsets, clamped to
0-255. "Filter frequency (FF)" sets chip 1 and the others the same way.
So, for a choir of different-sized voices: set chip 2 to -20, chip 3 to
-40, chip 4 to +15, and the wheel sweeps all four together, keeping the
spread. Like any parameter, the offsets can be automated and are saved
with the project.

- **REAPER + OSARA (Windows, macOS, Linux)**: the action "OSARA: View FX
  parameters for current track" lists every parameter with its value text.
  On Windows the plug-in's own window is also accessible: each control is
  a named slider or combo box with a help text, grouped (MIDI, Chip,
  Output), reached with Tab.
- **macOS (VoiceOver)**: JUCE implements macOS accessibility for its
  controls, so the same named sliders and combo boxes should read in
  VoiceOver, and keyboard focus is enabled for the plug-in window so Tab
  can reach them (not yet tried on a Mac with VoiceOver). Logic's
  "Controls" view (the plug-in header's View menu) is the generic
  alternative and needs nothing from JUCE.
- **Linux**: JUCE has no screen-reader support on Linux (no AT-SPI
  bridge), so the plug-in's own window is silent to Orca. Use the host's
  generic controls: REAPER's "UI" button, Ardour's "Edit with generic
  controls", Carla's "Edit" parameter view. Every value is there.

What the host can rely on (checked by pluginval and the C tests): state
save/restore through the project (`getStateInformation` /
`setStateInformation`, an XML of every parameter), 44.1/48/96 kHz and any
block size, offline render (same result as real time), sample-accurate
MIDI at note level, no allocation or lock on the audio thread (changing
Voices swaps between two prebuilt systems), and near silence with no notes
(the chip's idle floor, under -70 dBFS; it is the emulated chip's own
output, not gated).

## Ways to build it wrong

| Symptom | Cause | Fix |
|---|---|---|
| No `.vst3` anywhere, only a folder with `Contents/x86_64-win/` empty or a `.lib` | Built target `SSInger` (shared-code library) instead of `SSInger_VST3` | Build `--target SSInger_VST3` |
| Plugin loads but audio stutters / asserts | Debug build (forgot `--config Release`, or no `-DCMAKE_BUILD_TYPE=Release` with Ninja/Makefiles) | Rebuild Release, reinstall |
| New build, old behavior | DAW cached the previous plug-in | Remove, rescan empty, re-add, rescan |
| `ctest` reports "Not Run" | Test binary not built, or `-C` missing | Build `--target test_6850_midi`, run `ctest -C Release` |
| `LNK1104 cannot open .obj` / `LNK1136 corrupt file` mid-build | Stale `cl`/`MSBuild` processes from an interrupted build holding locks (then killed mid-compile) | Kill strays (`Get-Process cl,msbuild`), delete `build/SSInger/SSInger.dir`, rebuild. Build one `--target` per invocation. |
| Configure re-downloads JUCE every time | Deleted `build/` | Keep `build/` (git-ignored); reconfigure reuses `build/<dir>/_deps` |
| Linux configure: `Package 'alsa' not found`, `freetype2 not found` | JUCE's dev packages missing | The `apt install` line above |
| Linux build killed (`c++: fatal error: Killed signal`) | Out of memory on a small board | `--parallel 2` (or 1) |
| macOS: "SSInger is damaged and can't be opened", or the DAW does not list it | Quarantine flag on a downloaded, unnotarized bundle | `xattr -dr com.apple.quarantine <bundle>` |
| macOS: Logic does not list the AU | AudioUnit cache | `killall -9 AudioComponentRegistrar`, then rescan in Logic's Plug-in Manager; `auval -v aumu Ssng S263` should pass |
| macOS: a host on Intel (or Rosetta) refuses it | Built for one architecture only | Configure with `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`; `lipo -info` on the binary lists both |
| No CLAP target | `SSINGER_CLAP=OFF`, or the clap-juce-extensions fetch failed | Reconfigure with network, `-DSSINGER_CLAP=ON` |

## First sound (two-track method, per the patent)

1. Phoneme clip on **MIDI channel 1**, pitch clip on **channel 2**
   (defaults; "Phoneme channel" param moves the pair).
2. Channel 1 alone sings at ~30 Hz — near-inaudible without channel 2.
   That is the patented method, not a bug.
3. Key→phoneme layout: `note_map.txt` (reference; C4 = middle C). All 64
   chip phonemes: notes 36-89 (C2-F6) E to TH, 90-93 PA, and 94-102
   (A#6-F#7) M, N, NG, :A, :OH, :U, :UH, E2, LB.
4. No audio checklist: MIDI reaching the track (armed/monitored?) →
   channels 1+2 → pitch velocity > 0 (it owns volume; phoneme velocity is
   ignored) → Volume param up → pitch notes present (pitch latches, so one
   pitch note suffices until the next).

## Layout reference

`CMakeLists.txt` (JUCE 9.0.3 + clap-juce-extensions via FetchContent; the
formats per OS), `plugin/` (processor, editor), `emu/` (`mc6850.h`,
`ssinger_bus.h`, `ssinger_firmware.h`), `tests/`, `note_map.txt`,
`RESEARCH.md` (patent/tour/hardware analysis), `third_party/README.md`
(every third-party piece and its licence).
