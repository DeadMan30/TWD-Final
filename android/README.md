# Android APK Wrapper (`arm64-v8a`)

NativeActivity Gradle project that packages `libtwdgame.so` (built from the root
[CMakeLists.txt](../CMakeLists.txt) and [runtime/](../runtime/) PAL) into an Android APK.

- **Entry library:** `twdgame` (`ANativeActivity_onCreate` in [game/android_main.c](../game/android_main.c)).
- **Target ABI:** `arm64-v8a` (`minSdk 24`, `targetSdk 34`, OpenGL ES 3.0 + Vulkan runtime probing).
- **Assets:** Copy your own legal PC `Archives/` (or extracted files) to the app's external files directory on device (never commit assets to git).

Open `android/` in Android Studio or build an APK with Gradle (`gradle assembleDebug`).
