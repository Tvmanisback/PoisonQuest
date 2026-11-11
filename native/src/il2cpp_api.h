// Minimal IL2CPP C API surface used by libPoison.so.
// libil2cpp.so (Unity 6000, Android arm64) exports these symbols unmangled.
#pragma once

#include <cstddef>

struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppObject;
struct Il2CppThread;
struct Il2CppException;
struct MethodInfo;
struct FieldInfo;

struct Il2cppApi {
    Il2CppDomain* (*domain_get)();
    Il2CppThread* (*thread_attach)(Il2CppDomain*);
    const Il2CppAssembly** (*domain_get_assemblies)(const Il2CppDomain*, size_t*);
    const Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*);
    const char* (*image_get_name)(const Il2CppImage*);
    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char*, const char*);
    const MethodInfo* (*class_get_method_from_name)(Il2CppClass*, const char*, int);
    Il2CppObject* (*runtime_invoke)(const MethodInfo*, void*, void**, Il2CppException**);
    FieldInfo* (*class_get_field_from_name)(Il2CppClass*, const char*);
    void (*field_get_value)(Il2CppObject*, FieldInfo*, void*);
    void (*field_set_value)(Il2CppObject*, FieldInfo*, void*);
    void (*field_static_get_value)(FieldInfo*, void*);
    void* (*object_unbox)(Il2CppObject*);
};

// Leading layout of Unity 6000's MethodInfo: the first member is the native function
// pointer that il2cpp_runtime_invoke (and therefore Unity's message dispatch) calls.
struct MethodInfoHead {
    void* methodPointer;
};
