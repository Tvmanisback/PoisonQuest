// libPoison.so - native loader for the Poison menu on Quest (Unity 6000, IL2CPP, arm64).
//
// What it does:
//   1. Starts a background thread as soon as the library is loaded
//      (constructor, JNI_OnLoad, or an explicit call to Poison_Start()).
//   2. Waits for libil2cpp.so to be mapped and the IL2CPP domain to be ready.
//   3. Scans every loaded assembly for <POISON_NAMESPACE>.<POISON_CLASS>.
//   4. Attaches the thread to the VM and invokes the static, parameterless
//      <POISON_METHOD> (Plugin.Bootstrap). That method only subscribes to
//      Application.onBeforeRender, so the real menu start-up (creating
//      GameObjects, etc.) happens on Unity's main thread on the first frame.
//
// The managed menu code must be part of the game's IL2CPP metadata for step 3 to
// find it; if it is not, the loader logs that and keeps retrying until timeout.

#include "il2cpp_api.h"

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifndef POISON_NAMESPACE
#define POISON_NAMESPACE "Poison"
#endif
#ifndef POISON_CLASS
#define POISON_CLASS "Plugin"
#endif
#ifndef POISON_METHOD
#define POISON_METHOD "Bootstrap"
#endif

#define POISON_EXPORT extern "C" __attribute__((visibility("default")))

#define LOG_TAG "Poison"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kPollIntervalMs = 500;
constexpr int kTimeoutSeconds = 10 * 60;
constexpr int kSettleMs = 3000;  // grace period after the VM first looks ready
constexpr int kMaxInvokeAttempts = 3;

std::atomic<bool> g_started{false};

bool ResolveApi(Il2cppApi& api) {
    // RTLD_NOLOAD: only succeeds once Unity has already mapped libil2cpp.so.
    void* handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;

    bool ok = true;
#define RESOLVE(field, symbol)                                                   \
    api.field = reinterpret_cast<decltype(api.field)>(dlsym(handle, symbol));    \
    if (!api.field) {                                                            \
        LOGE("missing export %s", symbol);                                       \
        ok = false;                                                              \
    }
    RESOLVE(domain_get, "il2cpp_domain_get")
    RESOLVE(thread_attach, "il2cpp_thread_attach")
    RESOLVE(domain_get_assemblies, "il2cpp_domain_get_assemblies")
    RESOLVE(assembly_get_image, "il2cpp_assembly_get_image")
    RESOLVE(image_get_name, "il2cpp_image_get_name")
    RESOLVE(class_from_name, "il2cpp_class_from_name")
    RESOLVE(class_get_method_from_name, "il2cpp_class_get_method_from_name")
    RESOLVE(runtime_invoke, "il2cpp_runtime_invoke")
#undef RESOLVE
    dlclose(handle);  // balance the RTLD_NOLOAD refcount; the library stays mapped
    return ok;
}

// Returns the Poison entry class, or nullptr if it is not (yet) in any assembly.
Il2CppClass* FindEntryClass(const Il2cppApi& api, Il2CppDomain* domain, const char** image_out) {
    size_t count = 0;
    const Il2CppAssembly** assemblies = api.domain_get_assemblies(domain, &count);
    if (!assemblies) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* image = api.assembly_get_image(assemblies[i]);
        if (!image) continue;
        Il2CppClass* klass = api.class_from_name(image, POISON_NAMESPACE, POISON_CLASS);
        if (klass) {
            if (image_out) *image_out = api.image_get_name(image);
            return klass;
        }
    }
    return nullptr;
}

// True once the VM is far enough along that attaching a thread is safe.
bool VmLooksReady(const Il2cppApi& api, Il2CppDomain* domain) {
    size_t count = 0;
    const Il2CppAssembly** assemblies = api.domain_get_assemblies(domain, &count);
    if (!assemblies || count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* image = api.assembly_get_image(assemblies[i]);
        if (!image) continue;
        const char* name = api.image_get_name(image);
        if (name && std::strncmp(name, "UnityEngine.CoreModule", 22) == 0) return true;
    }
    return false;
}

void LoaderThread() {
    LOGI("loader thread started (target %s.%s::%s)", POISON_NAMESPACE, POISON_CLASS, POISON_METHOD);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTimeoutSeconds);
    auto sleep = [] { std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs)); };

    Il2cppApi api{};
    bool api_ready = false;
    bool settled = false;
    int attempts = 0;
    bool logged_missing_class = false;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!api_ready) {
            if (!ResolveApi(api)) {
                sleep();
                continue;
            }
            api_ready = true;
            LOGI("libil2cpp.so resolved");
        }

        Il2CppDomain* domain = api.domain_get();
        if (!domain || !VmLooksReady(api, domain)) {
            sleep();
            continue;
        }

        if (!settled) {
            settled = true;
            LOGI("IL2CPP domain ready, settling for %d ms", kSettleMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(kSettleMs));
            continue;
        }

        const char* image_name = nullptr;
        Il2CppClass* klass = FindEntryClass(api, domain, &image_name);
        if (!klass) {
            if (!logged_missing_class) {
                LOGW("%s.%s not found in any loaded assembly yet; retrying", POISON_NAMESPACE, POISON_CLASS);
                logged_missing_class = true;
            }
            sleep();
            continue;
        }

        const MethodInfo* method = api.class_get_method_from_name(klass, POISON_METHOD, 0);
        if (!method) {
            LOGE("%s.%s has no parameterless %s method", POISON_NAMESPACE, POISON_CLASS, POISON_METHOD);
            return;
        }

        api.thread_attach(domain);
        Il2CppException* exception = nullptr;
        api.runtime_invoke(method, nullptr, nullptr, &exception);
        if (exception) {
            LOGE("%s threw (attempt %d/%d)", POISON_METHOD, attempts + 1, kMaxInvokeAttempts);
            if (++attempts >= kMaxInvokeAttempts) return;
            sleep();
            continue;
        }

        LOGI("invoked %s.%s::%s from image '%s'", POISON_NAMESPACE, POISON_CLASS, POISON_METHOD,
             image_name ? image_name : "?");
        return;
    }

    LOGE("gave up after %d s: Poison entry point was never found", kTimeoutSeconds);
}

void Start() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    std::thread(LoaderThread).detach();
}

}  // namespace

// Runs when the library is dlopen()ed / System.loadLibrary("Poison") is called.
__attribute__((constructor)) static void PoisonOnLoad() { Start(); }

// Also start from JNI_OnLoad so patchers that rely on it behave the same way.
POISON_EXPORT jint JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    Start();
    return JNI_VERSION_1_6;
}

// For patchers/injectors that dlsym() an explicit entry point.
POISON_EXPORT void Poison_Start() { Start(); }

POISON_EXPORT const char* Poison_Version() { return "5.1.7-native"; }
