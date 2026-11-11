# libPoison.so (Quest, Unity 6000 / IL2CPP, arm64-v8a)

Native loader for the Poison menu. It does **not** contain the menu itself; it finds the
managed `Poison.Plugin` class inside the game's IL2CPP runtime and calls
`Plugin.Bootstrap()`, which defers the real start-up (`InjectDontDestroy`) to Unity's
main thread on the first rendered frame.

## How it works

1. Loaded via `System.loadLibrary("Poison")`, `dlopen`, or an injector calling `Poison_Start()`.
2. A background thread waits for `libil2cpp.so` and a ready IL2CPP domain
   (`UnityEngine.CoreModule` loaded, then a 3 s settle delay).
3. It scans all assemblies for `Poison.Plugin`, attaches itself to the VM and invokes the
   static, parameterless `Bootstrap` method. Logs go to logcat under the tag `Poison`.

Exports: `JNI_OnLoad`, `Poison_Start`, `Poison_Version`.

## Requirement: the managed code must be in the game's IL2CPP metadata

IL2CPP builds have no JIT and cannot load a `.dll` at runtime, so the C# in this repo has to
be compiled into the game's `GameAssembly` (`libil2cpp.so` + `global-metadata.dat`) by an
IL2CPP build, e.g. a Unity project that includes these sources. If the class is not present
the loader logs `Poison.Plugin not found in any loaded assembly yet` and retries for 10 minutes.
`Plugin.Bootstrap` is marked `[Preserve]` so IL2CPP stripping keeps it.

## Build

CI: `.github/workflows/build-libpoison.yml` builds `libPoison.so` with NDK r27c and uploads it
as the `libPoison-arm64-v8a` artifact (also attached to releases on `v*` tags).

Local (needs the Android NDK):

```
cmake -S native -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Loader logic test (host, no device): `bash native/test/run.sh`.

The entry point can be changed with `-DPOISON_NAMESPACE=... -DPOISON_CLASS=... -DPOISON_METHOD=...`.
