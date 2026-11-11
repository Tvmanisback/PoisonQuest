/*For SharpMonoInjector usage :3
 * Poison Menu  Plugin.cs
 * A community driven mod menu for Gorilla Tag with over 1000+ mods
 *For SharpMonoInjector usage :3
 * Copyright (C) 2026  Poison Software
 * https://github.com/heycanihavethis/Poison
 *For SharpMonoInjector usage :3
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *For SharpMonoInjector usage :3
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *For SharpMonoInjector usage :3
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>. g
 */

using Poison.Managers;
using Poison.Menu;
using UnityEngine;
using UnityEngine.Scripting;


namespace Poison
{
    [Preserve]
    public static class Plugin
    {
        private static bool bootstrapped;

        // Entry point for the native loader (libPoison.so on Quest / IL2CPP).
        // The loader calls this from a background thread, where creating GameObjects is
        // not allowed, so defer the real start-up to the first frame on the main thread.
        [Preserve]
        public static void Bootstrap()
        {
            if (bootstrapped) return;
            bootstrapped = true;
            Application.onBeforeRender += OnFirstFrame;
        }

        [Preserve]
        private static void OnFirstFrame()
        {
            Application.onBeforeRender -= OnFirstFrame;
            InjectDontDestroy();
        }

        // Don't merge these methods, it just doesn't work
        public static void Inject()
        {
            var go = new GameObject("Poison");
            go.AddComponent<Injector>();
        }

        public static void InjectDontDestroy()
        {
            var go = new GameObject("Poison");
            Object.DontDestroyOnLoad(go);
            go.AddComponent<Injector>();
        }

        private sealed class Injector : MonoBehaviour
        {
            private void Awake()
            {
                LogManager.SetLogger((Level level, string msg) =>
                {
                    switch (level)
                    {
                        case Level.Error:
                            Debug.LogError(msg);
                            break;
                        case Level.Warning:
                            Debug.LogWarning(msg);
                            break;
                        default:
                            Debug.Log(msg);
                            break;
                    }
                });

                Bootstrapper.Initialize();
            }

            private void OnDestroy() =>
                Main.UnloadMenu();
        }
    }
}
