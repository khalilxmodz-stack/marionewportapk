# Plumber Bros (native Android, C++)

A tiny, pure-native side-scroller: **C++17 + NativeActivity + EGL/GLES2**.
No Java/Kotlin code, no WebView, no emulator, no ROM, no external assets.
All graphics are colored rectangles drawn in code; the 3 levels are original layouts.

## Build the APK

Requirements: Android Studio (Koala or newer), JDK 17, Android SDK 34,
NDK (any recent version) and CMake 3.22.1 (install via *SDK Manager > SDK Tools*).

1. Open this folder in Android Studio and let Gradle sync
   (it downloads Gradle 8.7 from `gradle/wrapper/gradle-wrapper.properties`).
2. *Build > Build APK(s)*, or from a terminal (after `gradle wrapper` once if you have no wrapper jar):
   `./gradlew assembleDebug`
3. Install: `adb install app/build/outputs/apk/debug/app-debug.apk`

`./gradlew assembleRelease` also produces an installable APK (signed with the debug key).

## Controls
- Touch: left / right buttons (bottom-left), run+fire (B) and jump (A) bottom-right, pause/start top-center.
- Gamepad: D-pad or left stick, A = jump, B/X = run / fireball, Start = start/pause.
- Keyboard: arrows, Space/Z = jump, X/Shift = run/fire, Enter = start/pause.

## Gameplay included
Run/skid/jump with variable jump height, gravity, tile collision, ground/brick/question/hard blocks,
breakable bricks (big form), coins, pipes, Goombas, Koopas + kickable shells, mushroom and fire flower
(with fireballs), damage/shrink/death, camera scrolling, flagpole + time bonus, 3 levels,
score / coins / lives / timer, title, game over and win screens.

## Layout
- `app/src/main/cpp/game.cpp` all game logic + drawing (platform independent, ~800 lines)
- `app/src/main/cpp/main.cpp`  Android glue: EGL, GLES2 batch renderer, touch + gamepad input, 24 FPS loop
- `tools/host_test.cpp`        desktop smoke test: runs the real logic with a bot (see file header)

## Tweaks
- Render rate: `TARGET_FPS` in `main.cpp` (logic always ticks at 60 Hz, so physics are unaffected).
- Levels: `buildLevel()` in `game.cpp` uses helpers like `ground()`, `pipe()`, `q()`, `goomba()`.
- Name/label: `app/src/main/res/values/strings.xml` and the text in `game_draw()`.
