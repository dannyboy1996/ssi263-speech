# Third-party pieces and their licences

Nothing third-party is vendored in this folder: CMake fetches JUCE and
clap-juce-extensions at configure time (into the git-ignored `build/`), and
the optional 6502 core is cloned here by hand. This file lists every piece
that goes into an SSInger binary, and every tool that only checks one.

## In the plug-in binaries

| Piece | Version | Licence | In which binaries |
|---|---|---|---|
| SSInger's own code (`plugin/`, `emu/`, `tests/`) and the SSI-263 engine (`../src/csrc/ssi263*.c`) | this repo | MIT | all |
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.3 (tag, fetched) | AGPLv3, or the commercial JUCE 9 licence | all |
| Steinberg VST3 SDK (inside JUCE: `juce_audio_processors_headless/format_types/VST3_SDK`) | 3.8.0 | MIT (since VST3 SDK 3.8) | VST3 |
| Apple AudioUnitSDK (inside JUCE: `juce_audio_plugin_client/AU/AudioUnitSDK`) | as bundled by JUCE 9.0.3 | Apache-2.0 | AU (macOS) |
| LV2 headers, and serd / sord / sratom / lilv (inside JUCE: `.../format_types/LV2_SDK`) | as bundled by JUCE 9.0.3 | ISC | LV2 |
| [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) | commit `55525c98` (2026-09-12, JUCE 9 support) | MIT | CLAP |
| [CLAP](https://github.com/free-audio/clap) and [clap-helpers](https://github.com/free-audio/clap-helpers) (submodules of clap-juce-extensions) | as pinned by that commit | MIT | CLAP |
| JUCE's own bundled libraries that SSInger's modules compile in: zlib (zlib), libpng (PNG Reference Library v2), the Independent JPEG Group's jpeglib (IJG), HarfBuzz (Old MIT), SheenBidi (Apache-2.0), lunasvg + plutovg (MIT), libwebp (BSD-3-Clause), FLAC / Ogg / Vorbis / Opus / opusfile / libopusenc (BSD-3-Clause), PreSonus's pslextensions headers (public domain, VST3) | as bundled by JUCE 9.0.3 | as listed | all |

JUCE keeps a complete, versioned inventory of its bundled dependencies in
`JUCE.spdx.json` at the root of the fetched JUCE tree
(`build/<dir>/_deps/juce-src/`). JUCE's ASIO support (Windows) is off, so
Steinberg's ASIO SDK is not compiled in.

Every release download carries these notices: `licenses/` beside the
bundles, copied from the fetched trees by `../packaging/release.py`, with
`licenses/THIRD-PARTY-NOTICES.txt` as the index (this file ships there as
`licenses/README.md`), and `SOURCE.txt` naming the commit built.

Not used: the AAX SDK that JUCE also ships (no AAX target: BUILD.md, "Not
built: AAX"); JUCE's web browser and curl (turned off).

## What licence a binary carries

The source in this repo is MIT. A binary built with JUCE under its AGPLv3
option is **AGPLv3 as a whole**, in every format: VST3, AU, CLAP, LV2 and
Standalone alike (or under JUCE's commercial licence, if whoever builds it
holds one). Every other piece above is under a permissive licence that
AGPLv3 accepts (MIT, ISC, BSD, zlib, libpng, Apache-2.0), so no format
adds a conflict, and the same terms work on Windows, macOS and Linux.
Distributing those binaries means offering the corresponding source (this
repository at the commit built), as AGPLv3 asks.

Notes, none of them a blocker:

- Before VST3 SDK 3.8 the SDK was GPLv3 / proprietary; JUCE 9.0.3 bundles
  3.8.0, which is MIT.
- Apple's AudioUnitSDK is Apache-2.0, which is compatible with (A)GPLv3.
- "VST" is a Steinberg trademark and the VST3 usage guidelines
  (`VST3_Usage_Guidelines.pdf` in the SDK) ask for the VST logo/notice
  where the format is advertised; "Audio Units" and "AU" are Apple's.
- A Developer ID signature and notarization (macOS) and PACE signing (AAX)
  are not licences; they are distribution steps that need accounts
  (BUILD.md).

## Only in CI, never shipped

| Tool | Licence | Used for |
|---|---|---|
| [pluginval](https://github.com/Tracktion/pluginval) v1.0.4 (Tracktion) | GPL-3.0 | VST3 and AU validation |
| [clap-validator](https://github.com/free-audio/clap-validator) 0.4.1 | MIT | CLAP validation |
| `auval` | Apple, part of macOS | AU validation |
| `lv2ls` / `lv2info` (lilv-utils) | ISC | LV2 discovery check |

## floooh `chips` `m6502.h` — drop-in, MIT, not built by default

The 6502 core is **not** vendored here to keep the diff small. To enable
real 6502 execution on the emulated bus:

```
git clone --depth 1 https://github.com/floooh/chips.git third_party/chips
```

then configure with `-DSSINGER_HAVE_M6502=ON`. Only `chips/m6502.h`
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
