# xserver — Breadstick embedded X server

A fork of the X.Org/"lorie" X server (from termux-x11), built for Android and run **in-process**
by the BSL app. Produces `libXlorie.so` (arm64), vendored into `../Android/app/src/main/jniLibs`.

BSL-specific work lives in `build/termux-x11/app/src/main/cpp/lorie/` — notably the **rootless
per-window pipeline** (Composite-redirect each top-level window → its own AHardwareBuffer →
handed to the host so each guest window is its own DeX window) and the resize/keyboard/clipboard
integration. JNI is rebranded to `io.breadstick.x11` (kept stable; do not rename — it binds the
native symbols).

Build: CMake + Ninja with the x86_64 NDK r28c clang under `qemu-user-static` on an arm64 host:
```
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/.../android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -S build/termux-x11/app/src/main/cpp -B /tmp/lorie-build
cmake --build /tmp/lorie-build --target Xlorie
```
Then strip + copy `libXlorie.so` into `../Android/app/src/main/jniLibs/arm64-v8a/`.
