# libPoison.so (Quest, Unity 6000 / IL2CPP, arm64-v8a)

Native library for Gorilla Tag on Quest. It contains a C++ port of the core Poison movement
mods and, optionally, starts the managed C# menu if that is part of the game's IL2CPP build.

## What is ported

The full menu is ~85,000 lines of C# built on Harmony, Photon and 179 game DLL references, and
IL2CPP cannot load a `.dll` at runtime, so it cannot be turned into a `.so` mechanically. The
self-only movement mods from `Mods/Movement.cs` are re-implemented natively (15 mods):

| Key | Movement.cs mod | Behaviour |
|-----|-----------------|-----------|
| `speed` (+`maxspeed`, `jumpmulti`) | SpeedBoost | sets `GTPlayer.maxJumpSpeed` / `jumpMultiplier` |
| `fly` | Fly | right primary: move along head forward, zero velocity |
| `triggerfly` | TriggerFly | right trigger: velocity along head forward; release stops |
| `joystickfly` | JoystickFly | left stick: head forward/right, normalized |
| `dash` | Dash | right primary rising edge: velocity boost |
| `reversevelocity` | ReverseVelocity | right primary: flip velocity, 0.5 s cooldown |
| `updown` | UpAndDown | trigger up / grip down, gravity cancelled |
| `leftright` | LeftAndRight | trigger left / grip right (body axis) |
| `forwardback` | ForwardsAndBackwards | trigger forward / grip back (body axis) |
| `featherfalling` | FeatherFalling | damp velocity, clamp fall speed to -2 on ground |
| `funmove` | FunMove | velocity += velocity * dt |
| `strafe` | Strafe | horizontal velocity = body forward * maxJumpSpeed |
| `dynamicstrafe` | DynamicStrafe | steer horizontal velocity along body forward |
| `groundhelper` | GroundHelper | grip: cancel upward velocity |
| `gravity=low\|zero\|high` | Low/Zero/HighGravity | `Rigidbody.AddForce(..., Acceleration)` |

`flyspeed` sets the speed for the fly, dash and axis mods. Everything is off by default.
Settings live in a text file that is created on first run and hot-reloaded (about once per second):

```
/sdcard/Android/data/<game package>/files/PoisonMenu/native.cfg
```

(`POISON_CONFIG=<path>` overrides the location.) Logs go to logcat under the tag `Poison`.

### Not ported

Mods that need Harmony patches, Photon/network state, other players' rigs, custom
GameObjects/UI, file I/O systems or the menu UI itself (platforms, grappling hooks, rig mods,
guns, projectiles, visuals, safety/anti-report, etc.). Those depend on game internals that can
only be hooked reliably once a native hooking layer exists and can be tested on a device.

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
`ControllerInputPoller.rightControllerPrimaryButton/rightGrab/rightControllerIndexFloat/leftControllerPrimary2DAxis`, `Rigidbody.set_linearVelocity`) match the
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
