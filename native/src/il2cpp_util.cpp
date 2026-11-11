#include "il2cpp_util.h"

#include <cstdio>
#include <cstring>

namespace il2 {

namespace {
Il2cppApi g_api{};
Il2CppDomain* g_domain = nullptr;
}  // namespace

void Init(const Il2cppApi& api, Il2CppDomain* domain) {
    g_api = api;
    g_domain = domain;
}

const Il2cppApi& Api() { return g_api; }

Il2CppClass* FindClass(const char* ns, const char* name) {
    size_t count = 0;
    const Il2CppAssembly** assemblies = g_api.domain_get_assemblies(g_domain, &count);
    if (!assemblies) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* image = g_api.assembly_get_image(assemblies[i]);
        if (!image) continue;
        if (Il2CppClass* klass = g_api.class_from_name(image, ns, name)) return klass;
    }
    return nullptr;
}

const MethodInfo* Method(Il2CppClass* klass, const char* name, int argc) {
    return klass ? g_api.class_get_method_from_name(klass, name, argc) : nullptr;
}

FieldInfo* Field(Il2CppClass* klass, const char* name) {
    return klass ? g_api.class_get_field_from_name(klass, name) : nullptr;
}

bool Call(const MethodInfo* method, void* self, void** args, Il2CppObject** ret) {
    if (!method) return false;
    Il2CppException* exception = nullptr;
    Il2CppObject* result = g_api.runtime_invoke(method, self, args, &exception);
    if (exception) return false;
    if (ret) *ret = result;
    return true;
}

Il2CppObject* StaticInstance(Il2CppClass* klass) {
    if (!klass) return nullptr;
    if (const MethodInfo* getter = Method(klass, "get_Instance", 0)) {
        Il2CppObject* result = nullptr;
        if (Call(getter, nullptr, nullptr, &result) && result) return result;
    }
    static const char* const kNames[] = {"Instance", "instance", "_instance", "<Instance>k__BackingField"};
    for (const char* name : kNames) {
        if (FieldInfo* field = Field(klass, name)) {
            Il2CppObject* result = nullptr;
            g_api.field_static_get_value(field, &result);
            if (result) return result;
        }
    }
    return nullptr;
}

Il2CppObject* GetObject(Il2CppClass* klass, Il2CppObject* self, const char* name) {
    if (!klass || !self) return nullptr;
    if (FieldInfo* field = Field(klass, name)) {
        Il2CppObject* result = nullptr;
        g_api.field_get_value(self, field, &result);
        return result;
    }
    char getter[96];
    std::snprintf(getter, sizeof(getter), "get_%s", name);
    Il2CppObject* result = nullptr;
    Call(Method(klass, getter, 0), self, nullptr, &result);
    return result;
}

bool GetBool(Il2CppClass* klass, Il2CppObject* self, const char* name, bool* out) {
    if (!klass || !self) return false;
    if (FieldInfo* field = Field(klass, name)) {
        bool value = false;
        g_api.field_get_value(self, field, &value);
        *out = value;
        return true;
    }
    char getter[96];
    std::snprintf(getter, sizeof(getter), "get_%s", name);
    Il2CppObject* boxed = nullptr;
    if (!Call(Method(klass, getter, 0), self, nullptr, &boxed) || !boxed) return false;
    *out = *static_cast<bool*>(g_api.object_unbox(boxed));
    return true;
}

bool SetFloat(Il2CppClass* klass, Il2CppObject* self, const char* name, float value) {
    if (!klass || !self) return false;
    if (FieldInfo* field = Field(klass, name)) {
        g_api.field_set_value(self, field, &value);
        return true;
    }
    char setter[96];
    std::snprintf(setter, sizeof(setter), "set_%s", name);
    void* args[1] = {&value};
    return Call(Method(klass, setter, 1), self, args, nullptr);
}

bool CallReturnVec3(const MethodInfo* method, void* self, Vec3* out) {
    Il2CppObject* boxed = nullptr;
    if (!Call(method, self, nullptr, &boxed) || !boxed) return false;
    std::memcpy(out, g_api.object_unbox(boxed), sizeof(Vec3));
    return true;
}

bool CallReturnFloat(const MethodInfo* method, void* self, float* out) {
    Il2CppObject* boxed = nullptr;
    if (!Call(method, self, nullptr, &boxed) || !boxed) return false;
    std::memcpy(out, g_api.object_unbox(boxed), sizeof(float));
    return true;
}

}  // namespace il2
