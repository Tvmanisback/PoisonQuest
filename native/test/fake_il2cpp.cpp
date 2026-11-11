// Fake libil2cpp.so for host testing the loader and native mods.
//
// It models just enough of IL2CPP: classes/fields/methods looked up by name (with parent
// lookup), boxed value returns, MethodInfo with methodPointer at offset 0 (so the loader's
// pointer-swap hook works), and a handful of Gorilla Tag / Unity types.
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

struct Vec3 { float x, y, z; };

struct Box { void* klass; void* monitor; char data[16]; };
struct Obj { void* klass; void* monitor; char data[64]; };

struct Il2CppClass;
struct FieldInfo {
    const char* name;
    size_t off;
    size_t size;
    bool isStatic;
    alignas(8) char staticData[8];
};
using Impl = std::function<void*(Obj*, void**, Box*)>;
struct MethodInfo {
    void* methodPointer;          // offset 0, like Unity 6000
    void* virtualMethodPointer;
    void* invoker;
    const char* name;
    int argc;
    Impl impl;
};
struct Il2CppClass {
    const char* ns;
    const char* name;
    Il2CppClass* parent;
    int image;  // 0 = UnityEngine.CoreModule.dll, 1 = Assembly-CSharp.dll
    std::vector<FieldInfo*> fields;
    std::vector<MethodInfo*> methods;
};

namespace {

int g_domain, g_asm[2], g_img[2];
const void* g_assemblies[2] = {&g_asm[0], &g_asm[1]};
std::atomic<int> g_attach{0};
std::atomic<int> g_bootstrap{0};
std::atomic<int> g_updates{0};
std::vector<Il2CppClass*> g_classes;

// Game state.
Obj g_player, g_tagger, g_poller, g_rigidbody, g_headCollider, g_headTransform, g_playerTransform;
Obj g_bodyCollider, g_bodyTransform;
Vec3 g_playerPos{0, 0, 0}, g_velocity{9, 9, 9}, g_lastForce{0, 0, 0};
Vec3 g_headForward{0, 0, 1}, g_headRight{1, 0, 0}, g_bodyForward{0, 0, 1}, g_bodyRight{1, 0, 0};
float g_time = 100.0f;
bool g_grounded = false;
int g_forceCalls = 0, g_lastMode = -1;
Box g_boxes[8];
int g_boxIdx = 0;

Box* NewBox() { return &g_boxes[g_boxIdx++ % 8]; }

Il2CppClass* AddClass(const char* ns, const char* name, int image, Il2CppClass* parent = nullptr) {
    auto* c = new Il2CppClass{ns, name, parent, image, {}, {}};
    g_classes.push_back(c);
    return c;
}
FieldInfo* AddField(Il2CppClass* c, const char* name, size_t off, size_t size, bool isStatic = false) {
    auto* f = new FieldInfo{name, off, size, isStatic, {}};
    c->fields.push_back(f);
    return f;
}
MethodInfo* AddMethod(Il2CppClass* c, const char* name, int argc, Impl impl) {
    auto* m = new MethodInfo{nullptr, nullptr, nullptr, name, argc, std::move(impl)};
    c->methods.push_back(m);
    return m;
}

void FakeLateUpdate(void*, const void*) { ++g_updates; }
MethodInfo* g_lateUpdate = nullptr;

void Build() {
    // --- UnityEngine.CoreModule ---
    Il2CppClass* component = AddClass("UnityEngine", "Component", 0);
    Il2CppClass* collider = AddClass("UnityEngine", "Collider", 0, component);
    Il2CppClass* transform = AddClass("UnityEngine", "Transform", 0, component);
    Il2CppClass* rigidbody = AddClass("UnityEngine", "Rigidbody", 0);
    Il2CppClass* time = AddClass("UnityEngine", "Time", 0);
    Il2CppClass* physics = AddClass("UnityEngine", "Physics", 0);
    (void)collider;

    AddMethod(component, "get_transform", 0, [](Obj* self, void**, Box*) -> void* {
        if (self == &g_headCollider) return &g_headTransform;
        if (self == &g_bodyCollider) return &g_bodyTransform;
        return &g_playerTransform;
    });
    AddMethod(transform, "get_forward", 0, [](Obj* self, void**, Box* r) -> void* {
        std::memcpy(r->data, self == &g_bodyTransform ? &g_bodyForward : &g_headForward, sizeof(Vec3));
        return r;
    });
    AddMethod(transform, "get_right", 0, [](Obj* self, void**, Box* r) -> void* {
        std::memcpy(r->data, self == &g_bodyTransform ? &g_bodyRight : &g_headRight, sizeof(Vec3));
        return r;
    });
    AddMethod(transform, "get_position", 0, [](Obj*, void**, Box* r) -> void* {
        std::memcpy(r->data, &g_playerPos, sizeof(Vec3));
        return r;
    });
    AddMethod(transform, "set_position", 1, [](Obj*, void** a, Box*) -> void* {
        std::memcpy(&g_playerPos, a[0], sizeof(Vec3));
        return nullptr;
    });
    AddMethod(rigidbody, "AddForce", 2, [](Obj*, void** a, Box*) -> void* {
        std::memcpy(&g_lastForce, a[0], sizeof(Vec3));
        g_lastMode = *static_cast<int*>(a[1]);
        ++g_forceCalls;
        return nullptr;
    });
    AddMethod(rigidbody, "get_linearVelocity", 0, [](Obj*, void**, Box* r) -> void* {
        std::memcpy(r->data, &g_velocity, sizeof(Vec3));
        return r;
    });
    AddMethod(rigidbody, "set_linearVelocity", 1, [](Obj*, void** a, Box*) -> void* {
        std::memcpy(&g_velocity, a[0], sizeof(Vec3));
        return nullptr;
    });
    AddMethod(time, "get_deltaTime", 0, [](Obj*, void**, Box* r) -> void* {
        float dt = 0.02f;
        std::memcpy(r->data, &dt, sizeof(float));
        return r;
    });
    AddMethod(time, "get_time", 0, [](Obj*, void**, Box* r) -> void* {
        std::memcpy(r->data, &g_time, sizeof(float));
        return r;
    });
    AddMethod(time, "get_fixedDeltaTime", 0, [](Obj*, void**, Box* r) -> void* {
        float dt = 0.02f;
        std::memcpy(r->data, &dt, sizeof(float));
        return r;
    });
    AddMethod(physics, "get_gravity", 0, [](Obj*, void**, Box* r) -> void* {
        Vec3 g{0, -9.81f, 0};
        std::memcpy(r->data, &g, sizeof(Vec3));
        return r;
    });

    // --- Assembly-CSharp ---
    Il2CppClass* gtPlayer = AddClass("GorillaLocomotion", "GTPlayer", 1, component);
    AddField(gtPlayer, "maxJumpSpeed", 0, 4);
    AddField(gtPlayer, "jumpMultiplier", 4, 4);
    AddMethod(gtPlayer, "get_Instance", 0, [](Obj*, void**, Box*) -> void* { return &g_player; });

    Il2CppClass* tagger = AddClass("", "GorillaTagger", 1);
    AddField(tagger, "rigidbody", 0, 8);
    AddField(tagger, "headCollider", 8, 8);
    AddField(tagger, "bodyCollider", 16, 8);
    AddMethod(tagger, "IsGrounded", 1, [](Obj*, void**, Box* r) -> void* {
        r->data[0] = g_grounded ? 1 : 0;
        return r;
    });
    AddMethod(tagger, "IsGrounded", 0, [](Obj*, void**, Box* r) -> void* {
        r->data[0] = g_grounded ? 1 : 0;
        return r;
    });
    // Singleton exposed as a static field instead of get_Instance, to cover that path too.
    FieldInfo* inst = AddField(tagger, "Instance", 0, 8, true);
    void* taggerPtr = &g_tagger;
    std::memcpy(inst->staticData, &taggerPtr, sizeof(void*));
    g_lateUpdate = AddMethod(tagger, "LateUpdate", 0, nullptr);
    g_lateUpdate->methodPointer = reinterpret_cast<void*>(&FakeLateUpdate);

    Il2CppClass* poller = AddClass("", "ControllerInputPoller", 1);
    AddField(poller, "rightControllerPrimaryButton", 0, 1);
    AddField(poller, "rightGrab", 1, 1);
    AddField(poller, "rightControllerIndexFloat", 4, 4);
    AddField(poller, "leftControllerPrimary2DAxis", 8, 8);
    FieldInfo* pinst = AddField(poller, "instance", 0, 8, true);
    void* pollerPtr = &g_poller;
    std::memcpy(pinst->staticData, &pollerPtr, sizeof(void*));

    Il2CppClass* plugin = AddClass("Poison", "Plugin", 1);
    AddMethod(plugin, "Bootstrap", 0, [](Obj*, void**, Box*) -> void* {
        ++g_bootstrap;
        return nullptr;
    });

    // Object wiring.
    void* rb = &g_rigidbody;
    void* head = &g_headCollider;
    std::memcpy(g_tagger.data + 0, &rb, 8);
    std::memcpy(g_tagger.data + 8, &head, 8);
    void* body = &g_bodyCollider;
    std::memcpy(g_tagger.data + 16, &body, 8);
    *reinterpret_cast<float*>(g_player.data + 0) = 6.5f;
    *reinterpret_cast<float*>(g_player.data + 4) = 1.1f;
}

struct Init { Init() { Build(); } } g_init;

}  // namespace

EXPORT void* il2cpp_domain_get() { return &g_domain; }
EXPORT void* il2cpp_thread_attach(void*) { ++g_attach; return nullptr; }
EXPORT const void** il2cpp_domain_get_assemblies(const void*, size_t* n) {
    *n = 2;
    return g_assemblies;
}
EXPORT const void* il2cpp_assembly_get_image(const void* a) {
    return a == &g_asm[0] ? static_cast<const void*>(&g_img[0]) : static_cast<const void*>(&g_img[1]);
}
EXPORT const char* il2cpp_image_get_name(const void* i) {
    return i == &g_img[0] ? "UnityEngine.CoreModule.dll" : "Assembly-CSharp.dll";
}
EXPORT void* il2cpp_class_from_name(const void* img, const char* ns, const char* name) {
    int idx = img == &g_img[0] ? 0 : 1;
    for (Il2CppClass* c : g_classes)
        if (c->image == idx && !std::strcmp(c->ns, ns) && !std::strcmp(c->name, name)) return c;
    return nullptr;
}
EXPORT const void* il2cpp_class_get_method_from_name(void* k, const char* name, int argc) {
    for (Il2CppClass* c = static_cast<Il2CppClass*>(k); c; c = c->parent)
        for (MethodInfo* m : c->methods)
            if (!std::strcmp(m->name, name) && m->argc == argc) return m;
    return nullptr;
}
EXPORT void* il2cpp_class_get_field_from_name(void* k, const char* name) {
    for (Il2CppClass* c = static_cast<Il2CppClass*>(k); c; c = c->parent)
        for (FieldInfo* f : c->fields)
            if (!std::strcmp(f->name, name)) return f;
    return nullptr;
}
EXPORT void il2cpp_field_get_value(void* obj, void* f, void* out) {
    auto* field = static_cast<FieldInfo*>(f);
    std::memcpy(out, static_cast<Obj*>(obj)->data + field->off, field->size);
}
EXPORT void il2cpp_field_set_value(void* obj, void* f, void* in) {
    auto* field = static_cast<FieldInfo*>(f);
    std::memcpy(static_cast<Obj*>(obj)->data + field->off, in, field->size);
}
EXPORT void il2cpp_field_static_get_value(void* f, void* out) {
    auto* field = static_cast<FieldInfo*>(f);
    std::memcpy(out, field->staticData, field->size);
}
EXPORT void* il2cpp_object_unbox(void* o) { return static_cast<Box*>(o)->data; }
EXPORT void* il2cpp_runtime_invoke(const void* mi, void* self, void** args, void** exc) {
    if (exc) *exc = nullptr;
    auto* m = static_cast<const MethodInfo*>(mi);
    if (!m->impl) {  // engine-style call through methodPointer
        reinterpret_cast<void (*)(void*, const void*)>(m->methodPointer)(self, m);
        return nullptr;
    }
    return m->impl(static_cast<Obj*>(self), args, NewBox());
}

// ---- Test hooks ----
EXPORT int fake_bootstrap_count() { return g_bootstrap.load(); }
EXPORT int fake_attach_count() { return g_attach.load(); }
EXPORT int fake_update_count() { return g_updates.load(); }
// What Unity does every frame: dispatch the message through MethodInfo::methodPointer.
EXPORT void fake_engine_update() {
    reinterpret_cast<void (*)(void*, const void*)>(g_lateUpdate->methodPointer)(&g_tagger, g_lateUpdate);
}
EXPORT void fake_set_primary(int v) { g_poller.data[0] = v ? 1 : 0; }
EXPORT void fake_set_grab(int v) { g_poller.data[1] = v ? 1 : 0; }
EXPORT void fake_set_trigger(float v) { *reinterpret_cast<float*>(g_poller.data + 4) = v; }
EXPORT void fake_set_joystick(float x, float y) {
    float j[2] = {x, y};
    std::memcpy(g_poller.data + 8, j, sizeof(j));
}
EXPORT void fake_set_head_axes(float fx, float fy, float fz, float rx, float ry, float rz) {
    g_headForward = {fx, fy, fz};
    g_headRight = {rx, ry, rz};
}
EXPORT void fake_set_body_axes(float fx, float fy, float fz, float rx, float ry, float rz) {
    g_bodyForward = {fx, fy, fz};
    g_bodyRight = {rx, ry, rz};
}
EXPORT void fake_set_time(float t) { g_time = t; }
EXPORT void fake_set_grounded(int v) { g_grounded = v != 0; }
EXPORT void fake_set_forward(float x, float y, float z) { g_headForward = {x, y, z}; }
EXPORT void fake_set_player_state(float maxSpeed, float mult) {
    *reinterpret_cast<float*>(g_player.data + 0) = maxSpeed;
    *reinterpret_cast<float*>(g_player.data + 4) = mult;
}
EXPORT void fake_get_player_state(float* out) {
    out[0] = *reinterpret_cast<float*>(g_player.data + 0);
    out[1] = *reinterpret_cast<float*>(g_player.data + 4);
}
EXPORT void fake_get_position(float* out) { std::memcpy(out, &g_playerPos, sizeof(Vec3)); }
EXPORT void fake_set_position(float x, float y, float z) { g_playerPos = {x, y, z}; }
EXPORT void fake_get_velocity(float* out) { std::memcpy(out, &g_velocity, sizeof(Vec3)); }
EXPORT void fake_set_velocity(float x, float y, float z) { g_velocity = {x, y, z}; }
EXPORT int fake_force_calls() { return g_forceCalls; }
EXPORT int fake_last_mode() { return g_lastMode; }
EXPORT void fake_get_last_force(float* out) { std::memcpy(out, &g_lastForce, sizeof(Vec3)); }
