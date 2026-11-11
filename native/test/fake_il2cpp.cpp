// Fake libil2cpp.so for host testing the loader. Mimics just enough of the C API:
// two assemblies, the Poison class living in the second one, and a counter for invokes.
#include <atomic>
#include <cstddef>
#include <cstring>

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {
int g_domain, g_asm_a, g_asm_b, g_img_a, g_img_b, g_class, g_method;
const void* g_assemblies[2] = {&g_asm_a, &g_asm_b};
std::atomic<int> g_attach{0};
std::atomic<int> g_invoke{0};
}  // namespace

EXPORT void* il2cpp_domain_get() { return &g_domain; }
EXPORT void* il2cpp_thread_attach(void*) { ++g_attach; return nullptr; }
EXPORT const void** il2cpp_domain_get_assemblies(const void*, size_t* n) {
    *n = 2;
    return g_assemblies;
}
EXPORT const void* il2cpp_assembly_get_image(const void* a) {
    return a == &g_asm_a ? static_cast<const void*>(&g_img_a) : static_cast<const void*>(&g_img_b);
}
EXPORT const char* il2cpp_image_get_name(const void* i) {
    return i == &g_img_a ? "UnityEngine.CoreModule.dll" : "Assembly-CSharp.dll";
}
EXPORT void* il2cpp_class_from_name(const void* img, const char* ns, const char* name) {
    if (img == &g_img_b && !std::strcmp(ns, "Poison") && !std::strcmp(name, "Plugin")) return &g_class;
    return nullptr;
}
EXPORT const void* il2cpp_class_get_method_from_name(void* k, const char* name, int argc) {
    if (k == &g_class && !std::strcmp(name, "Bootstrap") && argc == 0) return &g_method;
    return nullptr;
}
EXPORT void* il2cpp_runtime_invoke(const void* m, void*, void**, void** exc) {
    if (m == &g_method) ++g_invoke;
    if (exc) *exc = nullptr;
    return nullptr;
}

// Test hooks.
EXPORT int fake_invoke_count() { return g_invoke.load(); }
EXPORT int fake_attach_count() { return g_attach.load(); }
