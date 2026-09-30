import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Everything the APK carries besides code is staged by build_android.sh at the repository root into
// build/android/assets: the unit's firmware (never in the repository; the APK is a release artifact, as the NVDA
// add-on that also carries it), the licences, and z180emu's GPL source.  The native library, from the same sources
// as the Linux and Windows builds, is dropped under jniLibs.  Gradle only checks both are there.
val repoRoot = rootProject.file("../../..")
val stagedAssets = File(repoRoot, "build/android/assets")
val abis = listOf("arm64-v8a", "armeabi-v7a", "x86_64")
val verifyNativeBuild = tasks.register("verifyNativeBuild") {
    doLast {
        for (abi in abis) {
            val library = file("src/main/jniLibs/$abi/libssi263speech.so")
            check(library.isFile) { "Build the $abi library with `sh build_android.sh` first" }
        }
        for (name in listOf("firmware/BL2ENG.BNS", "firmware/bl2_2003_warm.state", "licenses/DISTRIBUTION.txt",
                            "licenses/z180emu-GPL-2.0.txt", "source/ssi263-speech-source.tgz")) {
            check(File(stagedAssets, name).isFile) { "build/android/assets/$name is missing: run `sh build_android.sh`" }
        }
    }
}
tasks.matching { it.name == "preBuild" }.configureEach { dependsOn(verifyNativeBuild) }

android {
    namespace = "com.ssi263speech.tts"
    compileSdk = 35

    defaultConfig {
        // Permanent once shipped: Android treats a different id as a different app.
        applicationId = "com.ssi263speech.tts"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.7.0"

        ndk {
            abiFilters += abis
        }
    }

    // Kotlin sources live under src/main/kotlin.
    sourceSets["main"].java.srcDirs("src/main/kotlin")
    sourceSets["main"].assets.srcDir(stagedAssets)

    // The firmware and state are read as they are: no compression, so they copy out of the APK quickly.
    androidResources {
        noCompress += listOf("BNS", "state", "tgz")
    }

    // Release signing, as outspoken's and TGSpeechBox's builds: a `signing.properties` beside settings.gradle.kts,
    // ignored by Git, naming the key -- STORE_FILE, STORE_PASSWORD, KEY_ALIAS, KEY_PASSWORD.  Without it the release
    // build still succeeds and stays unsigned.  The same key must sign every future release.
    val signingProperties = rootProject.file("signing.properties")
    if (signingProperties.isFile) {
        val keys = Properties().apply { signingProperties.inputStream().use { load(it) } }
        signingConfigs {
            create("release") {
                storeFile = file(keys.getProperty("STORE_FILE"))
                storePassword = keys.getProperty("STORE_PASSWORD")
                keyAlias = keys.getProperty("KEY_ALIAS")
                keyPassword = keys.getProperty("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (signingProperties.isFile) signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
