// Native ports of the core Poison movement mods (Movement.cs).
#pragma once

namespace poison {

// Resolves the game classes and installs the per-frame hook. Returns true once the hook
// is live. Safe to call repeatedly until it succeeds; call from an IL2CPP-attached thread.
bool InstallMods();

}  // namespace poison
