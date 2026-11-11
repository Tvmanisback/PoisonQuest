# libPoison.so (Quest, Unity 6000 / IL2CPP, arm64-v8a)

Native library for Gorilla Tag on Quest. It contains a C++ port of the core Poison movement
mods and, optionally, starts the managed C# menu if that is part of the game's IL2CPP build.

## What is ported

The full menu is ~85,000 lines of C# built on Harmony, Photon and 179 game DLL references, and
IL2CPP cannot load a `.dll` at runtime, so it cannot be turned into a `.so` mechanically. The
core mods from `Mods/Movement.cs` are re-implemented natively:

| Mod | Behaviour | Config |
|-----|-----------|--------|
| Speed boost | sets `GTPlayer.maxJumpSpeed` / `jumpMultiplier` each frame | `speed`, `maxspeed`, `jumpmulti` |
| Fly | while right primary is held, move along the head's forward and zero velocity | `fly`, `flyspeed` |
| Gravity | low / zero / high via `Rigidbody.AddForce(..., Acceleration)` | `gravity=none\|low\|zero\|high` |

Everything is off by default. Settings live in a text file that is created on first run and
re-read about once per second:

```
/sdcard/Android/data/<game package>/files/PoisonMenu/native.cfg
```

```
speed=0
maxspeed=7.5
jumpmulti=1.1
fly=0
flyspeed=10
gravity=none
```

(`POISON_CONFIG=<path>` overrides the location.) Logs go to logcat under the tag `Poison`.

## How it works

1. Loaded via `System.loadLibrary("Poison")`, `dlopen`, or an injector calling `Poison_Start()`.
2. A background thread waits for `libil2cpp.so` and a ready IL2CPP domain
   (`UnityEngine.CoreModule` loaded, then a 3 s settle delay) and attaches to the VM.
3. `mods.cpp` swaps `MethodInfo::methodPointer` of `GorillaTagger.LateUpdate` (falling back to
   `Update` / `FixedUpdate`). Unity dispatches messages through `il2cpp_runtime_invoke`, so the
   replacement runs on the main thread every frame, calls the original, then applies the mods.
4. If `Poison.Plugin` exists in the game's IL2CPP metadata (C# menu compiled into the build),
   `Plugin.Bootstrap()` is invoked too; it defers menu start-up to the first rendered frame.

Exports: `JNI_OnLoad`, `Poison_Start`, `Poison_Version`.

Assumptions about the game that can only be confirmed on a device: class and member names
(`GorillaLocomotion.GTPlayer`, `GorillaTagger.rigidbody/headCollider`,
`ControllerInputPoller.rightControllerPrimaryButton`, `Rigidbody.set_linearVelocity`) match the
names used by the C# in this repo, and Unity 6000's `MethodInfo` keeps `methodPointer` first.
If something is missing the mod is skipped and logged.

## Build

CI: `.github/workflows/build-libpoison.yml` runs the host test, builds with NDK r27c and uploads
`libPoison.so` as the `libPoison-arm64-v8a` artifact (also attached to releases on `v*` tags).

Local (needs the Android NDK):

```
cmake -S native -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Host test (no device): `bash native/test/run.sh`. It runs the real loader and mods against a fake
`libil2cpp.so` and checks the hook, speed, fly, gravity and live config reload.

The managed entry point can be changed with
`-DPOISON_NAMESPACE=... -DPOISON_CLASS=... -DPOISON_METHOD=...`.
