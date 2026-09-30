# The Braille Lite 2000 voice for Android

An Android text-to-speech engine -- a system voice for TalkBack and every other app -- that speaks with the emulated
Blazie Braille Lite 2000: the unit's own June 2003 firmware on z180emu's Z180, driving the SSI-263 model. The native
part is the same C as the NVDA add-on's library and the Linux speech-dispatcher module (`src/csrc`), cross-built with
the NDK; its PCM is theirs, byte for byte (the tests below prove it on the desktop and on a phone). English and
Spanish, each once its firmware is imported.

The layout, the settings screen and its accessibility patterns come from outspoken's Android app; the stop handling
also borrows TGSpeechBox's.

## The firmware: imported, never shipped

The APK carries no firmware -- this app is the one place it cannot ship -- so each user imports their own, as
outspoken and Panthera take their engine data. Setup's "Import firmware…" (or `am start -n
com.ssi263speech.tts/.SettingsActivity --es import <path|content: URI>`, which skips the confirm
dialog; `--ez removefirmware true` removes it) takes the list below. A path under `/sdcard` is refused by scoped
storage (EACCES); from adb, hand over the file's MediaStore URI with a grant:

    adb shell "content query --uri content://media/external/file --projection _id --where \"_display_name='blt2000.exe'\""
    adb shell am start -n com.ssi263speech.tts/.SettingsActivity -d content://media/external/file/<id> \
        --grant-read-uri-permission --es import content://media/external/file/<id>

It takes:

- a zip, with the firmware at its top or one folder down; the NVDA add-on (`.nvda-addon`: `synthDrivers/_ssi263_blazie/`);
- one file: a `.BNS`, or an update program (`.exe`/`.com`) holding the image, raw or as a zip behind its code
  (`blt2000.exe`), also inside a zip.

Files are known by content, never by name (`src/csrc/blazie/bl_firmware.c`): the ROM image (`F3 C3 xx xx FF
"COPYRIGHT"`) anywhere in a file, and only the releases on the list, by the sha256 of their image (Tomi,
2026-09-30) -- each one booted through its state recipe and heard before it was listed:

| Release | Found on | Image sha256 |
|---|---|---|
| English, June 5, 2003 revision | "FS june2003": `blt2000.exe` and its `BL2ENG.BNS` | `ff8f30ec…` |
| English, ONCE's September 20, 2000 revision | ONCE's "braillehablado" disk: `blite2000/BL2ENG.BNS` | `c840112f…` |
| Spanish, ONCE's September 20, 2000 revision | ONCE's "braillehablado" disk: `blite2000/BL2SPA.BNS` | `eedd606e…` |

Any other image is refused in words: a Braille Lite 2000 release not on the list (another country's may lay its
memory out differently, and its recipe is not known), and another Blazie unit's firmware (Braille 'n Speak, Type 'n
Speak, Braille Lite 18 and 40), which `bl_create`'s firmware sites also refuse. A unit's state (786432 bytes, whatever
its name) is never imported, alone or in a zip: the app always makes its own. Picked alone, it is refused with "This
is a state file, not firmware. Please import only firmware files, or zips containing them, with this tool." (Tomi's
words); in a zip beside firmware (the add-on), it is left out and said so. Refusals are shown in a dialog, kept on
the Setup page and announced to TalkBack.

The unit's battery-backed state (it holds what the firmware wrote, so it cannot ship either) is made on the phone
from the firmware (`bl_state.c`): the bns.c runs that made the shipped states, replayed -- byte-identical to them,
checked on the phone against the list's hash for that release. English takes a few seconds, Spanish about a minute
(1150 million Z180 instructions; 71 s for both on the A024); the progress bar moves every 10 million instructions and
TalkBack hears every fifth of the way, no closer than 5 s apart. Then the unit speaks once (`ssa_probe`) and only then
do the files replace what was there, in device-protected storage. Until then the service reports its languages as
missing data and CheckVoiceData fails.

## Build

    sh build_android.sh                                   # at the repository root
    cd src/platforms/android && ./gradlew assembleDebug assembleRelease
    python src/platforms/android/test/check_apk_no_firmware.py app/build/outputs/apk/release/app-release.apk

`build_android.sh` needs the NDK (`ANDROID_NDK_HOME`, or the newest under `$ANDROID_HOME/ndk`) and z180emu (`Z180EMU`,
or the `Z180EMU` key in `paths.local`, or `third_party/z180emu`). It writes the libraries to
`app/src/main/jniLibs/<abi>/` and everything else the APK carries to `build/android/assets/` at the repository root
-- the licences and, for z180emu's GPL, the complete source. None of that is committed. A developer build may carry
the firmware, by asking: `SSI263_ANDROID_BUNDLE_FIRMWARE=1` (with `SSI263_FIRMWARE`, default `firmware/blazie`, the
Spanish unit there or in `spanish/`); never distribute one. Gradle then needs `-Pssi263BundleFirmware=1` too: without
it, staged firmware stops the build (a leftover once rode into a plain `assembleDebug`). `check_apk_no_firmware.py` looks inside an APK (and the
source archive in it) for a ROM image and a unit's state by content and firmware or state files by name;
`--control <BL2ENG.BNS> <apk>` adds the firmware under a bland name and must fail, as must `--control
<bl2_2003_warm.state> <apk>`.

Release signing reads `signing.properties` beside `settings.gradle.kts` (gitignored), as outspoken's and
TGSpeechBox's builds: `STORE_FILE`, `STORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`. Without it a release stays unsigned.

The app is GPL-2.0-or-later as a whole, because of z180emu; the project's own code is MIT. The Setup page's
"Licenses and source" shows both, and the source archive inside the APK.

## Test

    python src/platforms/android/test/test_android_native.py              # desktop: the app's C against bl_voice
    sh build_android.sh --test arm64-v8a
    python src/platforms/android/test/test_android_native.py --adb        # ... and on the attached phone
    SSI263_ANDROID_TEST_BREAK=1 python src/platforms/android/test/test_android_native.py   # control: must FAIL
    python src/platforms/android/test/test_device_service.py [--rate 2.0] [--aloud]    # the installed app's service
    python src/platforms/android/test/test_import_native.py               # the import's native part, and the states
    SSI263_IMPORT_TEST_BREAK=1 python src/platforms/android/test/test_import_native.py     # control: must FAIL
    SSI263_IMPORT_TEST_BREAK=hash python src/platforms/android/test/test_import_native.py  # control: must FAIL
    cd src/platforms/android && ./gradlew testDebugUnitTest               # the import's layouts and words, on the JVM
    ./gradlew testDebugUnitTest -Pssi263ImportBreak=1                     # control: the layout cases must FAIL
    ./gradlew testDebugUnitTest -Pssi263ImportBreak=state                 # control: the state cases must FAIL

`test_import_native.py` makes every fixture at test time from the files in the firmware folder (`--firmware`, default
`$SSI263_FIRMWARE`, else `firmware/blazie`, with `spanish/`, `tns/` and `once2000/` -- ONCE's September 2000
`BL2ENG.BNS`, its cases skipped when absent): the listed releases written as they came, the list and its labels, an
update program and the 1998 layout, two unknown releases refused, TNSENG.TNS refused, states and noise not firmware;
the states made from the firmware alone must be the listed ones (the shipped ones byte for byte), speak every case of
the Android test as they do, and report their progress every 10 million instructions, steadily. Its controls hold the
wrong chord at the English warm reset, and drop the list (the unknown releases are then taken). The JVM tests
(`app/src/test`) play the native side with a fake and check the zip layouts (top, one folder down, the add-on, an
update program inside a zip), the refusals' words (unknown releases, other units, states alone, in a zip and beside
firmware) and the choices between releases.

The reference in both is the desktop library the NVDA add-on and Linux ship (`bl.dll` + `ssi263.dll` from
`build_board.py` / `build_native.py`, or `build/linux/libssi263speech.so`), driven the way `sd_ssi263.c` maps SSIP.

## Rate, pitch, volume

The app's sliders use the NVDA driver's scales (rate and pitch 0-100, 50 = the unit's factory rate 11 and pitch 16;
tone 0-26, factory 7; volume 0-100). An app's request carries rate and pitch as percentages (100 = normal); they go
onto SSIP's scale at 50 per doubling, through `sd_ssi263.c`'s `to100`, and on top of the slider (`cpp/ssa_map.c`).
Android applies the request's volume to its own audio track, so the engine's volume is the slider alone.

## Files

| File | What it does |
|---|---|
| `build.gradle.kts`, `settings.gradle.kts`, `gradle.properties` | The Gradle project (AGP 8.7, Kotlin 2.0), as outspoken's |
| `gradlew`, `gradlew.bat`, `gradle/wrapper/` | The Gradle wrapper (8.13) |
| `.gitignore` | Build output, `jniLibs/`, `local.properties`, `signing.properties` and keystores stay out of git |
| `app/build.gradle.kts` | The app: id `com.ssi263speech.tts`, minSdk 26, three ABIs; takes `build/android/assets` as its assets and refuses to build without the libraries and the staged files; release signing; JUnit for `src/test`, and the tests' control property |
| `app/src/main/AndroidManifest.xml` | The TTS service (direct-boot aware), the settings screen (also the framework's INSTALL_TTS_DATA), the two activities the TTS framework asks, and the file picker it may ask |
| `app/src/main/res/values/strings.xml` | The app's and the engine's names |
| `app/src/main/res/xml/tts_engine.xml` | Tells the framework which screen holds the engine's settings |
| `app/src/main/cpp/ssa_map.h`, `ssa_map.c` | An Android request's rate and pitch onto the voice's scales, the way `sd_ssi263.c` maps SSIP; the test's control switch |
| `app/src/main/cpp/ssa_engine.h`, `ssa_engine.c` | The front end in plain C around `bl_voice.h`: the voices, the boot settings, start, pull in chunks, stop (any thread) and cancel; `ssa_probe`, the import's last check |
| `app/src/main/cpp/ssa_jni.c` | The thin JNI bridge to `ssa_engine`, and to the import's native part (`bl_firmware.h`, `bl_state.h`) |
| `app/src/main/kotlin/com/ssi263speech/tts/SsiNative.kt` | The JNI declarations |
| `.../SsiData.kt` | The unit's files in device-protected storage: which voices are imported and as what, the import's move into place, removal; a developer build's bundled firmware |
| `.../FirmwareImport.kt` | What a source holds (no Android in it, so the JVM tests run it): zip layouts, the add-on, update programs, single files; the refusals in words |
| `.../SsiImport.kt` | The import on the phone: the source's bytes, the native judgement, the states made and checked, the unit made to speak, the files moved into place |
| `.../SsiSettings.kt` | The settings, in device-protected storage, read once per utterance |
| `.../SsiEngine.kt` | The one engine in the process: opens it, lists the voices, owns it for one utterance at a time |
| `.../SsiTtsService.kt` | The `TextToSpeechService`: voices and languages, the request's voice, rate and pitch, audio streamed block by block, stop |
| `.../SettingsActivity.kt` | The screen: Setup (the firmware's import and removal with progress, status, a preview, the system TTS settings, licences and source) and Voice settings; the adb test hooks |
| `.../SettingsWidgets.kt` | Headings, the accessible slider, radio buttons and check boxes, from outspoken |
| `.../PreviewPlayer.kt` | The preview: rendered through the engine, played on an AudioTrack, kept as `last-render.wav` |
| `.../TtsSelfTest.kt` | The service through Android's own client, bound by package name (the default engine untouched): a file render, or a stop |
| `.../CheckVoiceDataActivity.kt` | Answers the framework's voice-data check: a voice is there once its firmware is imported |
| `app/src/test/kotlin/.../FirmwareImportTest.kt` | The JVM tests of `FirmwareImport`, with a fake native side |
| `.../GetSampleTextActivity.kt` | The sample sentence the system's TTS settings speak |
| `licenses/DISTRIBUTION.txt` | The distribution notice the licences dialog shows first (GPL, the source, no firmware) |
| `licenses/Kotlin-LICENSE.txt`, `Kotlin-NOTICE.txt` | The Kotlin runtime's licence (the project's MIT licence and z180emu's GPL are added by `build_android.sh`) |
| `test/test_android_native.c` | The host-side test program: the app's C on the same chip, board, host and voice, each case's PCM hashed |
| `test/test_android_native.py` | Builds and runs it on the desktop (and over adb), and compares with `bl_voice` driven as `sd_ssi263` drives it |
| `test/test_import_native.py` | The import's native part: the list of releases, the refusals, and the states made on the device, against the listed ones, with their progress |
| `test/check_apk_no_firmware.py` | An APK carries no firmware or state, by content and by name; its controls |
| `test/test_device_service.py` | The installed app's TTS service on the phone, its file's PCM against the same reference |
