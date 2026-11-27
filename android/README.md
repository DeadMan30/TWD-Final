# Android APK Wrapper (`arm64-v8a`)

NativeActivity Gradle project that packages `libtwdgame.so` (built from the root
[CMakeLists.txt](../CMakeLists.txt) and [runtime/](../runtime/) PAL) into an Android APK.

- **Entry library:** `twdgame` (`ANativeActivity_onCreate` in [game/android_main.c](../game/android_main.c), backed by [runtime/android_entry.c](../runtime/android_entry.c)).
- **Target ABI:** `arm64-v8a` (`minSdk 24`, `targetSdk 34`, EGL + OpenGL ES 3.0 hardware VBO/shader rendering onto `ANativeWindow`).
- **Assets & key:** Copy your own legal PC `.ttarch2` archives into `/sdcard/Android/data/com.twdfinal.android/files/` (or `/Archives/` inside it, or internal files dir; never commit assets to git). Optionally place a one-line archive key in `archive_key.txt` in the same folder if encrypted.
- **Audio:** If you place arm64 `libfmod.so` and `libfmodstudio.so` in `app/src/main/jniLibs/arm64-v8a/` (or app files dir), [audio_shim.c](../runtime/audio_shim.c) loads them dynamically alongside `libOpenSLES.so`.

## Building

- **Gradle APK (`android/`):** Open `android/` in Android Studio or run `gradle assembleDebug` (requires Android SDK 34, CMake `3.22.1+`, and NDK installed).
- **Direct NDK `libtwdgame.so` build:**
  ```sh
  cmake -S . -B build-android \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build build-android -j
  ```
