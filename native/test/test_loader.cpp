// Host test: libPoison must wait for libil2cpp.so, find Poison.Plugin, and invoke
// Bootstrap exactly once, even if Poison_Start() is called again.
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

static int Fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 3) return Fail("usage: test_loader <libPoison.so> <libil2cpp.so>");

    // Load the loader first: il2cpp is not mapped yet, so it has to wait.
    void* poison = dlopen(argv[1], RTLD_NOW);
    if (!poison) return Fail(dlerror());
    using VersionFn = const char* (*)();
    using StartFn = void (*)();
    auto version = reinterpret_cast<VersionFn>(dlsym(poison, "Poison_Version"));
    auto start = reinterpret_cast<StartFn>(dlsym(poison, "Poison_Start"));
    if (!version || !start) return Fail("Poison_Version/Poison_Start not exported");
    std::printf("loaded %s\n", version());

    sleep(1);
    void* il2cpp = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!il2cpp) return Fail(dlerror());
    auto invokes = reinterpret_cast<int (*)()>(dlsym(il2cpp, "fake_invoke_count"));
    auto attaches = reinterpret_cast<int (*)()>(dlsym(il2cpp, "fake_attach_count"));

    for (int i = 0; i < 20 && invokes() == 0; ++i) sleep(1);
    if (invokes() != 1) return Fail("Bootstrap was not invoked exactly once");
    if (attaches() < 1) return Fail("thread was never attached");

    start();  // must be a no-op
    sleep(2);
    if (invokes() != 1) return Fail("Bootstrap invoked again after second Poison_Start()");

    std::printf("PASS (invokes=%d attaches=%d)\n", invokes(), attaches());
    return 0;
}
