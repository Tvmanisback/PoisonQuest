// Native ports of the Poison movement mods (Mods/Movement.cs), self-only mods.
//
// Per-frame execution: Unity dispatches MonoBehaviour messages through
// il2cpp_runtime_invoke, which calls MethodInfo::methodPointer. We swap that pointer for
// GorillaTagger's LateUpdate/Update so our tick runs on the main thread every frame, then
// call the original.
//
// Ported mods and their Movement.cs counterparts:
//   speed            SpeedBoost            (GTPlayer.maxJumpSpeed / jumpMultiplier)
//   fly              Fly                   (right primary: move along head forward)
//   triggerfly       TriggerFly            (right trigger: velocity along head forward)
//   joystickfly      JoystickFly           (left stick: head forward/right)
//   dash             Dash                  (right primary edge: velocity boost)
//   reversevelocity  ReverseVelocity       (right primary: flip velocity, 0.5 s cooldown)
//   updown           UpAndDown             (trigger up / grip down, no gravity)
//   leftright        LeftAndRight          (trigger left / grip right, no gravity)
//   forwardback      ForwardsAndBackwards  (trigger forward / grip back, no gravity)
//   featherfalling   FeatherFalling        (damp velocity, clamp fall speed on ground)
//   funmove          FunMove               (velocity += velocity * dt)
//   strafe           Strafe                (horizontal velocity = body forward * maxJumpSpeed)
//   dynamicstrafe    DynamicStrafe         (steer horizontal velocity to body forward)
//   groundhelper     GroundHelper          (grip: cancel upward velocity)
//   gravity          LowGravity / ZeroGravity / HighGravity
//
// Config (key=value, hot-reloaded): see WriteDefaultConfig().

#include "mods.h"

#include "il2cpp_util.h"

#include <android/log.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define LOG_TAG "Poison"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace poison {
namespace {

using il2::Vec2;
using il2::Vec3;

enum class Gravity { None, Low, Zero, High };

struct Config {
    bool speed = false;
    float maxSpeed = 7.5f;   // Movement.jspeed default
    float jumpMulti = 1.1f;  // Movement.jmulti default
    float flySpeed = 10.0f;  // Movement._flySpeed default
    Gravity gravity = Gravity::None;

    bool fly = false, triggerFly = false, joystickFly = false, dash = false, reverseVelocity = false;
    bool upDown = false, leftRight = false, forwardBack = false;
    bool featherFalling = false, funMove = false, strafe = false, dynamicStrafe = false, groundHelper = false;
};

struct BoolKey {
    const char* key;
    bool Config::*field;
};

const BoolKey kBoolKeys[] = {
    {"speed", &Config::speed},
    {"fly", &Config::fly},
    {"triggerfly", &Config::triggerFly},
    {"joystickfly", &Config::joystickFly},
    {"dash", &Config::dash},
    {"reversevelocity", &Config::reverseVelocity},
    {"updown", &Config::upDown},
    {"leftright", &Config::leftRight},
    {"forwardback", &Config::forwardBack},
    {"featherfalling", &Config::featherFalling},
    {"funmove", &Config::funMove},
    {"strafe", &Config::strafe},
    {"dynamicstrafe", &Config::dynamicStrafe},
    {"groundhelper", &Config::groundHelper},
};

bool AnyEnabled(const Config& c) {
    if (c.gravity != Gravity::None) return true;
    for (const BoolKey& k : kBoolKeys)
        if (c.*(k.field)) return true;
    return false;
}

struct Handles {
    Il2CppClass* gtPlayer = nullptr;
    Il2CppClass* tagger = nullptr;
    Il2CppClass* poller = nullptr;
    Il2CppClass* rigidbody = nullptr;
    const MethodInfo* getTransform = nullptr;    // Component.get_transform
    const MethodInfo* getForward = nullptr;      // Transform.get_forward
    const MethodInfo* getRight = nullptr;        // Transform.get_right
    const MethodInfo* getPosition = nullptr;     // Transform.get_position
    const MethodInfo* setPosition = nullptr;     // Transform.set_position
    const MethodInfo* addForce = nullptr;        // Rigidbody.AddForce(Vector3, ForceMode)
    const MethodInfo* getVelocity = nullptr;     // Rigidbody.get_linearVelocity
    const MethodInfo* setVelocity = nullptr;     // Rigidbody.set_linearVelocity
    const MethodInfo* getDeltaTime = nullptr;    // Time.get_deltaTime
    const MethodInfo* getFixedDelta = nullptr;   // Time.get_fixedDeltaTime
    const MethodInfo* getTime = nullptr;         // Time.get_time
    const MethodInfo* getGravity = nullptr;      // Physics.get_gravity
    const MethodInfo* isGrounded0 = nullptr;     // GorillaTagger.IsGrounded()
    const MethodInfo* isGrounded1 = nullptr;     // GorillaTagger.IsGrounded(float)
};

Handles g;
Config g_config;  // main-thread only after install
std::atomic<bool> g_installed{false};
void (*g_original)(void*, const void*) = nullptr;
char g_configPath[512];
long g_nextConfigCheckMs = 0;
long long g_configStamp = -1;
long g_reloadIntervalMs = 1000;

// Edge/cooldown state.
bool g_triggerFlyHeld = false, g_joystickFlyHeld = false, g_prevDash = false;
float g_nextReverseTime = 0.0f;

constexpr int kForceModeAcceleration = 5;

long NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void BuildConfigPath() {
    if (const char* interval = std::getenv("POISON_RELOAD_MS")) g_reloadIntervalMs = std::max(10L, std::atol(interval));
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
        "# 1 = on, 0 = off. Everything is off by default.\n"
        "speed=0\n"
        "maxspeed=7.5\n"
        "jumpmulti=1.1\n"
        "flyspeed=10\n"
        "fly=0\n"
        "triggerfly=0\n"
        "joystickfly=0\n"
        "dash=0\n"
        "reversevelocity=0\n"
        "updown=0\n"
        "leftright=0\n"
        "forwardback=0\n"
        "featherfalling=0\n"
        "funmove=0\n"
        "strafe=0\n"
        "dynamicstrafe=0\n"
        "groundhelper=0\n"
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
    for (const BoolKey& k : kBoolKeys) {
        if (!std::strcmp(key, k.key)) {
            c.*(k.field) = std::atoi(value) != 0;
            return;
        }
    }
    if (!std::strcmp(key, "maxspeed")) c.maxSpeed = static_cast<float>(std::atof(value));
    else if (!std::strcmp(key, "jumpmulti")) c.jumpMulti = static_cast<float>(std::atof(value));
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
        g_configStamp = -1;
        return;
    }
    long long stamp = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec +
                      static_cast<long long>(st.st_size);
    if (stamp == g_configStamp) return;
    g_configStamp = stamp;

    Config c;
    if (FILE* f = std::fopen(g_configPath, "r")) {
        char line[160];
        while (std::fgets(line, sizeof(line), f)) ParseLine(line, c);
        std::fclose(f);
    }
    g_config = c;
    int on = 0;
    for (const BoolKey& k : kBoolKeys) on += c.*(k.field) ? 1 : 0;
    LOGI("config reloaded: %d mod(s) on, gravity=%d, flyspeed=%.1f", on, static_cast<int>(c.gravity), c.flySpeed);
}

// ---- Per-frame context -------------------------------------------------------------

struct Frame {
    Il2CppObject* player = nullptr;
    Il2CppObject* tagger = nullptr;
    Il2CppObject* poller = nullptr;
    Il2CppObject* rb = nullptr;
    Il2CppObject* head = nullptr;   // GorillaTagger.headCollider
    Il2CppObject* body = nullptr;   // GorillaTagger.bodyCollider
    float dt = 0.0f;
    float time = 0.0f;
    // Inputs (Movement.cs rightPrimary / rightTrigger / rightGrab / leftJoystick).
    bool rightPrimary = false;
    bool rightGrab = false;
    float rightTrigger = 0.0f;
    Vec2 leftJoystick{0.0f, 0.0f};
};

bool Velocity(const Frame& f, Vec3* out) { return il2::CallReturnVec3(g.getVelocity, f.rb, out); }

void SetVelocity(const Frame& f, Vec3 v) {
    void* args[1] = {&v};
    il2::Call(g.setVelocity, f.rb, args, nullptr);
}

void AddVelocity(const Frame& f, Vec3 delta) {
    Vec3 v{};
    if (Velocity(f, &v)) SetVelocity(f, v + delta);
}

// collider.transform.{forward,right}
bool Axis(const MethodInfo* getter, Il2CppObject* collider, Vec3* out) {
    Il2CppObject* tr = nullptr;
    if (!collider || !il2::Call(g.getTransform, collider, nullptr, &tr) || !tr) return false;
    return il2::CallReturnVec3(getter, tr, out);
}

void AddForce(const Frame& f, Vec3 force) {
    int mode = kForceModeAcceleration;
    void* args[2] = {&force, &mode};
    il2::Call(g.addForce, f.rb, args, nullptr);
}

void ZeroGravity(const Frame& f) {
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    il2::CallReturnVec3(g.getGravity, nullptr, &gravity);
    AddForce(f, -gravity);
}

bool Grounded(const Frame& f, float tolerance, bool useTolerance) {
    bool grounded = false;
    Il2CppObject* boxed = nullptr;
    if (!useTolerance && g.isGrounded0) {
        if (il2::Call(g.isGrounded0, f.tagger, nullptr, &boxed) && boxed)
            grounded = *static_cast<bool*>(il2::Api().object_unbox(boxed));
    } else if (g.isGrounded1) {
        void* args[1] = {&tolerance};
        if (il2::Call(g.isGrounded1, f.tagger, args, &boxed) && boxed)
            grounded = *static_cast<bool*>(il2::Api().object_unbox(boxed));
    }
    return grounded;
}

// ---- Mods --------------------------------------------------------------------------

void ModSpeed(const Frame& f, const Config& c) {
    il2::SetFloat(g.gtPlayer, f.player, "maxJumpSpeed", c.maxSpeed);
    il2::SetFloat(g.gtPlayer, f.player, "jumpMultiplier", c.jumpMulti);
}

void ModGravity(const Frame& f, const Config& c) {
    switch (c.gravity) {
        case Gravity::Low: AddForce(f, {0.0f, 6.66f, 0.0f}); break;
        case Gravity::High: AddForce(f, {0.0f, -7.77f, 0.0f}); break;
        case Gravity::Zero: ZeroGravity(f); break;
        default: break;
    }
}

void ModFly(const Frame& f, const Config& c) {
    if (!f.rightPrimary) return;
    Vec3 forward{}, position{};
    Il2CppObject* playerTransform = nullptr;
    if (!Axis(g.getForward, f.head, &forward) || !il2::Call(g.getTransform, f.player, nullptr, &playerTransform) ||
        !playerTransform || !il2::CallReturnVec3(g.getPosition, playerTransform, &position))
        return;
    Vec3 target = position + forward * (f.dt * c.flySpeed);
    void* args[1] = {&target};
    il2::Call(g.setPosition, playerTransform, args, nullptr);
    SetVelocity(f, {0.0f, 0.0f, 0.0f});
}

void ModTriggerFly(const Frame& f, const Config& c) {
    bool held = f.rightTrigger > 0.5f;
    Vec3 forward{};
    if (held) {
        if (Axis(g.getForward, f.head, &forward)) SetVelocity(f, forward * c.flySpeed);
    } else if (g_triggerFlyHeld) {
        SetVelocity(f, {0.0f, 0.0f, 0.0f});
    }
    g_triggerFlyHeld = held;
}

void ModJoystickFly(const Frame& f, const Config& c) {
    const Vec2 joy = f.leftJoystick;
    bool held = std::fabs(joy.x) > 0.3f || std::fabs(joy.y) > 0.3f;
    if (held) {
        Vec3 forward{}, right{};
        if (Axis(g.getForward, f.head, &forward) && Axis(g.getRight, f.head, &right)) {
            Vec3 move = forward * joy.y + right * joy.x;
            float len = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
            if (len > 1e-6f) move = move * (1.0f / len);  // Vector3.normalized (zero stays zero)
            SetVelocity(f, move * c.flySpeed);
        }
    } else if (g_joystickFlyHeld) {
        SetVelocity(f, {0.0f, 0.0f, 0.0f});
    }
    g_joystickFlyHeld = held;
}

void ModDash(const Frame& f, const Config& c) {
    Vec3 forward{};
    if (f.rightPrimary && !g_prevDash && Axis(g.getForward, f.head, &forward)) AddVelocity(f, forward * c.flySpeed);
    g_prevDash = f.rightPrimary;
}

void ModReverseVelocity(const Frame& f) {
    if (f.time < g_nextReverseTime || !f.rightPrimary) return;
    Vec3 v{};
    if (Velocity(f, &v)) {
        SetVelocity(f, -v);
        g_nextReverseTime = f.time + 0.5f;  // Movement.revCooldown
    }
}

// UpAndDown / LeftAndRight / ForwardsAndBackwards share one shape.
void ModAxisMove(const Frame& f, const Config& c, int axis) {
    bool trigger = f.rightTrigger > 0.5f;
    if (!trigger && !f.rightGrab) return;
    ZeroGravity(f);
    Vec3 dir{0.0f, 1.0f, 0.0f};
    float triggerSign = 1.0f;  // direction applied for trigger; grip is the opposite
    if (axis == 1) {  // LeftAndRight: trigger = -right, grip = +right
        if (!Axis(g.getRight, f.body, &dir)) return;
        triggerSign = -1.0f;
    } else if (axis == 2) {  // ForwardsAndBackwards: trigger = +forward, grip = -forward
        if (!Axis(g.getForward, f.body, &dir)) return;
    }
    float step = f.dt * c.flySpeed * 3.0f;
    if (trigger) AddVelocity(f, dir * (step * triggerSign));
    if (f.rightGrab) AddVelocity(f, dir * (-step * triggerSign));
}

void ModFeatherFalling(const Frame& f) {
    Vec3 v{};
    float fixedDt = 0.02f;
    if (!Velocity(f, &v)) return;
    il2::CallReturnFloat(g.getFixedDelta, nullptr, &fixedDt);
    float mag = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (mag > 0.1f || !Grounded(f, 0.2f, true)) {
        v = v - (v * 4.0f) * fixedDt;
        SetVelocity(f, v);
        if (Grounded(f, 0.0f, false) && v.y < 0.0f) SetVelocity(f, {v.x, std::max(v.y, -2.0f), v.z});
    }
}

void ModFunMove(const Frame& f) {
    Vec3 v{};
    if (Velocity(f, &v)) SetVelocity(f, v + v * f.dt);
}

void ModStrafe(const Frame& f, bool dynamic) {
    Vec3 forward{}, v{};
    if (!Axis(g.getForward, f.body, &forward) || !Velocity(f, &v)) return;
    float power;
    if (dynamic) power = std::sqrt(v.x * v.x + v.z * v.z);
    else {
        float maxJump = 0.0f;
        if (!il2::GetValue(g.gtPlayer, f.player, "maxJumpSpeed", &maxJump, sizeof(float))) return;
        power = maxJump;
    }
    SetVelocity(f, {forward.x * power, v.y, forward.z * power});
}

void ModGroundHelper(const Frame& f) {
    Vec3 v{};
    if (f.rightGrab && Velocity(f, &v) && v.y > 0.0f) SetVelocity(f, {v.x, 0.0f, v.z});
}

void Tick() {
    long now = NowMs();
    if (now >= g_nextConfigCheckMs) {
        g_nextConfigCheckMs = now + g_reloadIntervalMs;
        LoadConfig();
    }
    const Config& c = g_config;
    if (!AnyEnabled(c)) return;

    Frame f;
    f.player = il2::StaticInstance(g.gtPlayer);
    f.tagger = il2::StaticInstance(g.tagger);
    if (!f.player || !f.tagger) return;
    f.rb = il2::GetObject(g.tagger, f.tagger, "rigidbody");
    f.head = il2::GetObject(g.tagger, f.tagger, "headCollider");
    f.body = il2::GetObject(g.tagger, f.tagger, "bodyCollider");
    il2::CallReturnFloat(g.getDeltaTime, nullptr, &f.dt);
    il2::CallReturnFloat(g.getTime, nullptr, &f.time);

    f.poller = il2::StaticInstance(g.poller);
    if (f.poller) {
        il2::GetBool(g.poller, f.poller, "rightControllerPrimaryButton", &f.rightPrimary);
        il2::GetBool(g.poller, f.poller, "rightGrab", &f.rightGrab);
        il2::GetValue(g.poller, f.poller, "rightControllerIndexFloat", &f.rightTrigger, sizeof(float));
        il2::GetValue(g.poller, f.poller, "leftControllerPrimary2DAxis", &f.leftJoystick, sizeof(Vec2));
    }

    if (c.speed) ModSpeed(f, c);
    if (!f.rb) return;  // everything below drives the rigidbody

    if (c.gravity != Gravity::None) ModGravity(f, c);
    if (c.fly) ModFly(f, c);
    if (c.triggerFly) ModTriggerFly(f, c);
    if (c.joystickFly) ModJoystickFly(f, c);
    if (c.dash) ModDash(f, c);
    if (c.reverseVelocity) ModReverseVelocity(f);
    if (c.upDown) ModAxisMove(f, c, 0);
    if (c.leftRight) ModAxisMove(f, c, 1);
    if (c.forwardBack) ModAxisMove(f, c, 2);
    if (c.featherFalling) ModFeatherFalling(f);
    if (c.funMove) ModFunMove(f);
    if (c.strafe) ModStrafe(f, false);
    if (c.dynamicStrafe) ModStrafe(f, true);
    if (c.groundHelper) ModGroundHelper(f);
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
    g.getRight = il2::Method(transform, "get_right", 0);
    g.getPosition = il2::Method(transform, "get_position", 0);
    g.setPosition = il2::Method(transform, "set_position", 1);
    g.addForce = il2::Method(g.rigidbody, "AddForce", 2);
    g.getVelocity = il2::Method(g.rigidbody, "get_linearVelocity", 0);
    g.setVelocity = il2::Method(g.rigidbody, "set_linearVelocity", 1);
    g.getDeltaTime = il2::Method(time, "get_deltaTime", 0);
    g.getFixedDelta = il2::Method(time, "get_fixedDeltaTime", 0);
    g.getTime = il2::Method(time, "get_time", 0);
    g.getGravity = il2::Method(physics, "get_gravity", 0);
    g.isGrounded0 = il2::Method(g.tagger, "IsGrounded", 0);
    g.isGrounded1 = il2::Method(g.tagger, "IsGrounded", 1);

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
    LOGI("native mods installed (15 mods)");
    return true;
}

}  // namespace poison
