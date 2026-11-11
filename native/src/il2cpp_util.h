// Small reflection helpers on top of the raw IL2CPP C API.
#pragma once

#include "il2cpp_api.h"

namespace il2 {

struct Vec3 {
    float x, y, z;
};

void Init(const Il2cppApi& api, Il2CppDomain* domain);
const Il2cppApi& Api();

// Searches every loaded assembly. Returns nullptr if the class does not exist (yet).
Il2CppClass* FindClass(const char* ns, const char* name);
const MethodInfo* Method(Il2CppClass* klass, const char* name, int argc);
FieldInfo* Field(Il2CppClass* klass, const char* name);

// Invokes a method; returns false if it threw. `ret` may be null.
bool Call(const MethodInfo* method, void* self, void** args, Il2CppObject** ret);

// Singleton access: get_Instance(), then static fields Instance / instance / _instance.
Il2CppObject* StaticInstance(Il2CppClass* klass);

// Reads a reference-typed member, either a field or a get_<name>() property.
Il2CppObject* GetObject(Il2CppClass* klass, Il2CppObject* self, const char* name);
bool GetBool(Il2CppClass* klass, Il2CppObject* self, const char* name, bool* out);
// Writes a float member, either a field or a set_<name>(float) property.
bool SetFloat(Il2CppClass* klass, Il2CppObject* self, const char* name, float value);

// Calls a method that returns a boxed value type and copies it out.
bool CallReturnVec3(const MethodInfo* method, void* self, Vec3* out);
bool CallReturnFloat(const MethodInfo* method, void* self, float* out);

}  // namespace il2
