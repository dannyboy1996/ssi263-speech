# The Braille Lite 2000 voice for Android

An Android text-to-speech engine -- a system voice for TalkBack and every other app -- that speaks with the emulated
Blazie Braille Lite 2000: the unit's own June 2003 firmware on z180emu's Z180, driving the SSI-263 model. The native
part is the same C as the NVDA add-on's library and the Linux speech-dispatcher module (`src/csrc`), cross-built with
the NDK; its PCM is theirs, byte for byte (the tests below prove it on the desktop and on a phone). English, and
Spanish when its firmware is built in.

The layout, the settings screen and its accessibility patterns come from outspoken's Android app; the stop handling
also borrows TGSpeechBox's.

## Build

    SSI263_FIRMWARE=<folder with BL2ENG.BNS + bl2_2003_warm.state> sh build_android.sh    # at the repository root
    cd src/platforms/android && ./gradlew assembleDebug

`build_android.sh` needs the NDK (`ANDROID_NDK_HOME`, or the newest under `$ANDROID_HOME/ndk`) and z180emu (`Z180EMU`,
or the `Z180EMU` key in `paths.local`, or `third_party/z180emu`). The firmware folder defaults to `firmware/blazie`;
the Spanish unit (`BL2SPA.BNS` + `bl2spa_fresh.state`) is taken from there or its `spanish/` folder when both files
are present. It writes the libraries to `app/src/main/jniLibs/<abi>/` and everything else the APK carries to
`build/android/assets/` at the repository root -- the firmware, the licences and, for z180emu's GPL, the complete
source. None of that is committed. The APK is a release artifact, as the add-on: it carries the firmware.

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
| `app/build.gradle.kts` | The app: id `com.ssi263speech.tts`, minSdk 26, three ABIs; takes `build/android/assets` as its assets and refuses to build without the libraries and the staged files; release signing |
| `app/src/main/AndroidManifest.xml` | The TTS service (direct-boot aware), the settings screen, and the two activities the TTS framework asks |
| `app/src/main/res/values/strings.xml` | The app's and the engine's names |
| `app/src/main/res/xml/tts_engine.xml` | Tells the framework which screen holds the engine's settings |
| `app/src/main/cpp/ssa_map.h`, `ssa_map.c` | An Android request's rate and pitch onto the voice's scales, the way `sd_ssi263.c` maps SSIP; the test's control switch |
| `app/src/main/cpp/ssa_engine.h`, `ssa_engine.c` | The front end in plain C around `bl_voice.h`: the voices, the boot settings, start, pull in chunks, stop (any thread) and cancel |
| `app/src/main/cpp/ssa_jni.c` | The thin JNI bridge to `ssa_engine` |
| `app/src/main/kotlin/com/ssi263speech/tts/SsiNative.kt` | The JNI declarations |
| `.../SsiData.kt` | Copies the unit's files out of the APK into device-protected storage, once per installed version |
| `.../SsiSettings.kt` | The settings, in device-protected storage, read once per utterance |
| `.../SsiEngine.kt` | The one engine in the process: opens it, lists the voices, owns it for one utterance at a time |
| `.../SsiTtsService.kt` | The `TextToSpeechService`: voices and languages, the request's voice, rate and pitch, audio streamed block by block, stop |
| `.../SettingsActivity.kt` | The screen: Setup (status, a preview, the system TTS settings, licences and source) and Voice settings; the adb test hooks |
| `.../SettingsWidgets.kt` | Headings, the accessible slider, radio buttons and check boxes, from outspoken |
| `.../PreviewPlayer.kt` | The preview: rendered through the engine, played on an AudioTrack, kept as `last-render.wav` |
| `.../TtsSelfTest.kt` | The service through Android's own client, bound by package name (the default engine untouched): a file render, or a stop |
| `.../CheckVoiceDataActivity.kt` | Answers the framework's voice-data check |
| `.../GetSampleTextActivity.kt` | The sample sentence the system's TTS settings speak |
| `licenses/DISTRIBUTION.txt` | The distribution notice the licences dialog shows first (GPL, the source, the firmware) |
| `licenses/Kotlin-LICENSE.txt`, `Kotlin-NOTICE.txt` | The Kotlin runtime's licence (the project's MIT licence and z180emu's GPL are added by `build_android.sh`) |
| `test/test_android_native.c` | The host-side test program: the app's C on the same chip, board, host and voice, each case's PCM hashed |
| `test/test_android_native.py` | Builds and runs it on the desktop (and over adb), and compares with `bl_voice` driven as `sd_ssi263` drives it |
| `test/test_device_service.py` | The installed app's TTS service on the phone, its file's PCM against the same reference |
