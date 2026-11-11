// Native ports of the core Poison movement mods.
//
// Per-frame execution: Unity dispatches MonoBehaviour messages through
// il2cpp_runtime_invoke, which calls MethodInfo::methodPointer. We swap that pointer for
// GorillaTagger's LateUpdate/Update so our tick runs on the main thread every frame, then
// call the original.
//
// Mods (same behaviour as Mods/Movement.cs):
//   speed    GTPlayer.maxJumpSpeed / jumpMultiplier         (SpeedBoost)
//   fly      move along the head's forward while right primary is held (Fly)
//   gravity  low / zero / high via Rigidbody.AddForce        (LowGravity / ZeroGravity / HighGravity)
//
// Config (key=value, re-read about once per second): see LoadConfig().

#include "mods.h"

#include "il2cpp_util.h"

#include <android/log.h>
#include <sys/stat.h>
#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define LOG_TAG "Poison"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace poison {
namespace {

enum class Gravity { None, Low, Zero, High };

struct Config {
    bool speed = false;
    float maxSpeed = 7.5f;    // Movement.jspeed default
    float jumpMulti = 1.1f;   // Movement.jmulti default
    bool fly = false;
    float flySpeed = 10.0f;   // Movement._flySpeed default
    Gravity gravity = Gravity::None;
};

struct Handles {
    Il2CppClass* gtPlayer = nullptr;
    Il2CppClass* tagger = nullptr;
    Il2CppClass* poller = nullptr;
    Il2CppClass* rigidbody = nullptr;
    const MethodInfo* getTransform = nullptr;   // Component.get_transform
    const MethodInfo* getForward = nullptr;     // Transform.get_forward
    const MethodInfo* getPosition = nullptr;    // Transform.get_position
    const MethodInfo* setPosition = nullptr;    // Transform.set_position
    const MethodInfo* addForce = nullptr;       // Rigidbody.AddForce(Vector3, ForceMode)
    const MethodInfo* setVelocity = nullptr;    // Rigidbody.set_linearVelocity
    const MethodInfo* getDeltaTime = nullptr;   // Time.get_deltaTime
    const MethodInfo* getGravity = nullptr;     // Physics.get_gravity
};

Handles g;
Config g_config;          // only touched on the main thread (tick) after install
std::atomic<bool> g_installed{false};
void (*g_original)(void*, const void*) = nullptr;
char g_configPath[512];
long g_nextConfigCheckMs = 0;
long g_configMtime = -1;

constexpr int kForceModeAcceleration = 5;

long NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void BuildConfigPath() {
    if (const char* override_path = std::getenv("POISON_CONFIG")) {
        std::snprintf(g_configPath, sizeof(g_configPath), "%s", override_path);
        return;
    }
    // App-private external storage: needs no permission. Package name = /proc/self/cmdline.
    char pkg[256] = "com.AnotherAxiom.GorillaTag";
    if (FILE* f = std::fopen("/proc/self/cmdline", "r")) {
        char buf[256] = {};
        if (std::fgets(buf, sizeof(buf), f) && buf[0]) std::snprintf(pkg, sizeof(pkg), "%s", buf);
        std::fclose(f);
    }
    char dir[400];
    std::snprintf(dir, sizeof(dir), "/sdcard/Android/data/%s/files", pkg);
    mkdir(dir, 0775);
    std::snprintf(dir, sizeof(dir), "/sdcard/Android/data/%s/files/PoisonMenu", pkg);
    mkdir(dir, 0775);
    std::snprintf(g_configPath, sizeof(g_configPath), "%s/native.cfg", dir);
}

void WriteDefaultConfig() {
    FILE* f = std::fopen(g_configPath, "w");
    if (!f) return;
    std::fputs(
        "# libPoison native mods. Edit while the game runs; applied within ~1 second.\n"
        "speed=0\n"
        "maxspeed=7.5\n"
        "jumpmulti=1.1\n"
        "fly=0\n"
        "flyspeed=10\n"
        "# gravity: none | low | zero | high\n"
        "gravity=none\n",
        f);
    std::fclose(f);
}

void ParseLine(char* line, Config& c) {
    char* eq = std::strchr(line, '=');
    if (!eq || line[0] == '#') return;
    *eq = 0;
    const char* key = line;
    char* value = eq + 1;
    value[std::strcspn(value, "\r\n")] = 0;
    if (!std::strcmp(key, "speed")) c.speed = std::atoi(value) != 0;
    else if (!std::strcmp(key, "maxspeed")) c.maxSpeed = static_cast<float>(std::atof(value));
    else if (!std::strcmp(key, "jumpmulti")) c.jumpMulti = static_cast<float>(std::atof(value));
    else if (!std::strcmp(key, "fly")) c.fly = std::atoi(value) != 0;
    else if (!std::strcmp(key, "flyspeed")) c.flySpeed = static_cast<float>(std::atof(value));
    else if (!std::strcmp(key, "gravity")) {
        if (!std::strcmp(value, "low")) c.gravity = Gravity::Low;
        else if (!std::strcmp(value, "zero")) c.gravity = Gravity::Zero;
        else if (!std::strcmp(value, "high")) c.gravity = Gravity::High;
        else c.gravity = Gravity::None;
    }
}

void LoadConfig() {
    struct stat st{};
    if (stat(g_configPath, &st) != 0) {
        WriteDefaultConfig();
        g_config = Config{};
        g_configMtime = -1;
        return;
    }
    long mtime = static_cast<long>(st.st_mtime) * 1000003L + static_cast<long>(st.st_size);
    if (mtime == g_configMtime) return;
    g_configMtime = mtime;

    Config c;
    if (FILE* f = std::fopen(g_configPath, "r")) {
        char line[160];
        while (std::fgets(line, sizeof(line), f)) ParseLine(line, c);
        std::fclose(f);
    }
    g_config = c;
    LOGI("config: speed=%d(%.2f/%.2f) fly=%d(%.1f) gravity=%d", c.speed, c.maxSpeed, c.jumpMulti, c.fly,
         c.flySpeed, static_cast<int>(c.gravity));
}

bool AddForce(Il2CppObject* rb, il2::Vec3 force) {
    int mode = kForceModeAcceleration;
    void* args[2] = {&force, &mode};
    return il2::Call(g.addForce, rb, args, nullptr);
}

void Tick() {
    long now = NowMs();
    if (now >= g_nextConfigCheckMs) {
        g_nextConfigCheckMs = now + 1000;
        LoadConfig();
    }
    const Config& c = g_config;
    if (!c.speed && !c.fly && c.gravity == Gravity::None) return;

    Il2CppObject* player = il2::StaticInstance(g.gtPlayer);
    Il2CppObject* tagger = il2::StaticInstance(g.tagger);
    if (!player || !tagger) return;

    if (c.speed) {
        il2::SetFloat(g.gtPlayer, player, "maxJumpSpeed", c.maxSpeed);
        il2::SetFloat(g.gtPlayer, player, "jumpMultiplier", c.jumpMulti);
    }

    Il2CppObject* rb = il2::GetObject(g.tagger, tagger, "rigidbody");

    if (c.gravity != Gravity::None && rb) {
        il2::Vec3 force{0.0f, 0.0f, 0.0f};
        switch (c.gravity) {
            case Gravity::Low: force = {0.0f, 6.66f, 0.0f}; break;
            case Gravity::High: force = {0.0f, -7.77f, 0.0f}; break;
            case Gravity::Zero: {
                il2::Vec3 gravity{0.0f, -9.81f, 0.0f};
                il2::CallReturnVec3(g.getGravity, nullptr, &gravity);
                force = {-gravity.x, -gravity.y, -gravity.z};
                break;
            }
            default: break;
        }
        AddForce(rb, force);
    }

    if (c.fly) {
        Il2CppObject* poller = il2::StaticInstance(g.poller);
        bool primary = false;
        if (poller && il2::GetBool(g.poller, poller, "rightControllerPrimaryButton", &primary) && primary) {
            Il2CppObject* head = il2::GetObject(g.tagger, tagger, "headCollider");
            Il2CppObject* headTransform = nullptr;
            Il2CppObject* playerTransform = nullptr;
            il2::Vec3 forward{}, position{};
            float dt = 0.0f;
            if (head && il2::Call(g.getTransform, head, nullptr, &headTransform) && headTransform &&
                il2::Call(g.getTransform, player, nullptr, &playerTransform) && playerTransform &&
                il2::CallReturnVec3(g.getForward, headTransform, &forward) &&
                il2::CallReturnVec3(g.getPosition, playerTransform, &position) &&
                il2::CallReturnFloat(g.getDeltaTime, nullptr, &dt)) {
                float step = dt * c.flySpeed;
                il2::Vec3 target{position.x + forward.x * step, position.y + forward.y * step,
                                 position.z + forward.z * step};
                void* args[1] = {&target};
                il2::Call(g.setPosition, playerTransform, args, nullptr);
                if (rb) {
                    il2::Vec3 zero{0.0f, 0.0f, 0.0f};
                    void* vargs[1] = {&zero};
                    il2::Call(g.setVelocity, rb, vargs, nullptr);
                }
            }
        }
    }
}

// Replacement for GorillaTagger.LateUpdate/Update (instance, no args).
void HookedUpdate(void* self, const void* method) {
    if (g_original) g_original(self, method);
    Tick();
}

}  // namespace

bool InstallMods() {
    if (g_installed.load()) return true;

    g.gtPlayer = il2::FindClass("GorillaLocomotion", "GTPlayer");
    g.tagger = il2::FindClass("", "GorillaTagger");
    g.poller = il2::FindClass("", "ControllerInputPoller");
    Il2CppClass* component = il2::FindClass("UnityEngine", "Component");
    Il2CppClass* transform = il2::FindClass("UnityEngine", "Transform");
    g.rigidbody = il2::FindClass("UnityEngine", "Rigidbody");
    Il2CppClass* time = il2::FindClass("UnityEngine", "Time");
    Il2CppClass* physics = il2::FindClass("UnityEngine", "Physics");
    if (!g.gtPlayer || !g.tagger || !component || !transform || !g.rigidbody) return false;

    g.getTransform = il2::Method(component, "get_transform", 0);
    g.getForward = il2::Method(transform, "get_forward", 0);
    g.getPosition = il2::Method(transform, "get_position", 0);
    g.setPosition = il2::Method(transform, "set_position", 1);
    g.addForce = il2::Method(g.rigidbody, "AddForce", 2);
    g.setVelocity = il2::Method(g.rigidbody, "set_linearVelocity", 1);
    g.getDeltaTime = il2::Method(time, "get_deltaTime", 0);
    g.getGravity = il2::Method(physics, "get_gravity", 0);

    // Pick the first per-frame message GorillaTagger implements.
    const MethodInfo* target = nullptr;
    for (const char* name : {"LateUpdate", "Update", "FixedUpdate"}) {
        target = il2::Method(g.tagger, name, 0);
        if (target) break;
    }
    if (!target) {
        LOGW("GorillaTagger has no LateUpdate/Update/FixedUpdate; mods not installed");
        return false;
    }

    BuildConfigPath();
    LOGI("config file: %s", g_configPath);

    auto* head = reinterpret_cast<MethodInfoHead*>(const_cast<MethodInfo*>(target));
    g_original = reinterpret_cast<void (*)(void*, const void*)>(head->methodPointer);
    if (!g_original) return false;
    head->methodPointer = reinterpret_cast<void*>(&HookedUpdate);

    g_installed.store(true);
    LOGI("native mods installed (speed, fly, gravity)");
    return true;
}

}  // namespace poison
