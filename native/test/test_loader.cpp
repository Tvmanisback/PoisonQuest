// Host test: libPoison must wait for libil2cpp.so, invoke the managed entry once, hook the
// per-frame update, and apply the native mods (speed, fly, gravity) from the config file.
#include <dlfcn.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::fprintf(stderr, "FAIL line %d: ", __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);            \
            std::fputc('\n', stderr);                     \
            ++g_failures;                                 \
        }                                                 \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

static void WriteConfig(const char* path, const char* text) {
    FILE* f = std::fopen(path, "w");
    std::fputs(text, f);
    std::fclose(f);
}

template <class T>
static T Sym(void* lib, const char* name) {
    void* s = dlsym(lib, name);
    if (!s) {
        std::fprintf(stderr, "missing symbol %s\n", name);
        std::exit(2);
    }
    return reinterpret_cast<T>(s);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: test_loader <libPoison.so> <libil2cpp.so> <config path>\n");
        return 2;
    }
    const char* cfg = argv[3];
    WriteConfig(cfg, "speed=1\nmaxspeed=9\njumpmulti=2\nfly=1\nflyspeed=10\ngravity=low\n");
    setenv("POISON_CONFIG", cfg, 1);

    // Load the loader first: il2cpp is not mapped yet, so it has to wait.
    void* poison = dlopen(argv[1], RTLD_NOW);
    if (!poison) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    auto version = Sym<const char* (*)()>(poison, "Poison_Version");
    auto start = Sym<void (*)()>(poison, "Poison_Start");
    std::printf("loaded %s\n", version());

    sleep(1);
    void* il = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!il) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    auto bootstraps = Sym<int (*)()>(il, "fake_bootstrap_count");
    auto updates = Sym<int (*)()>(il, "fake_update_count");
    auto engineUpdate = Sym<void (*)()>(il, "fake_engine_update");
    auto setPrimary = Sym<void (*)(int)>(il, "fake_set_primary");
    auto setForward = Sym<void (*)(float, float, float)>(il, "fake_set_forward");
    auto setPlayer = Sym<void (*)(float, float)>(il, "fake_set_player_state");
    auto getPlayer = Sym<void (*)(float*)>(il, "fake_get_player_state");
    auto getPos = Sym<void (*)(float*)>(il, "fake_get_position");
    auto setPos = Sym<void (*)(float, float, float)>(il, "fake_set_position");
    auto getVel = Sym<void (*)(float*)>(il, "fake_get_velocity");
    auto setVel = Sym<void (*)(float, float, float)>(il, "fake_set_velocity");
    auto forceCalls = Sym<int (*)()>(il, "fake_force_calls");
    auto lastMode = Sym<int (*)()>(il, "fake_last_mode");
    auto lastForce = Sym<void (*)(float*)>(il, "fake_get_last_force");

    // Frame loop until the hook is live (speed boost shows up in GTPlayer).
    float p[2] = {0, 0};
    int frames = 0;
    for (int i = 0; i < 40 && !Near(p[0], 9.0f); ++i) {
        engineUpdate();
        ++frames;
        getPlayer(p);
        if (!Near(p[0], 9.0f)) sleep(1);
    }
    CHECK(bootstraps() == 1, "managed Bootstrap invoked %d times (want 1)", bootstraps());
    CHECK(Near(p[0], 9.0f) && Near(p[1], 2.0f), "speed boost: got %.2f/%.2f want 9/2", p[0], p[1]);
    CHECK(updates() == frames, "original LateUpdate ran %d times for %d frames", updates(), frames);

    // Gravity low: Rigidbody.AddForce(up * 6.66, Acceleration).
    float f[3];
    lastForce(f);
    CHECK(forceCalls() >= 1 && Near(f[0], 0) && Near(f[1], 6.66f) && Near(f[2], 0) && lastMode() == 5,
          "low gravity force (%.2f,%.2f,%.2f) mode %d", f[0], f[1], f[2], lastMode());

    // Fly: button released -> no movement; held -> forward * dt * speed (0.02 * 10 = 0.2), velocity zeroed.
    setPos(0, 0, 0);
    setVel(5, 5, 5);
    setForward(0, 0, 1);
    setPrimary(0);
    engineUpdate();
    float pos[3];
    getPos(pos);
    CHECK(Near(pos[2], 0.0f), "moved without button: z=%.3f", pos[2]);
    setPrimary(1);
    engineUpdate();
    getPos(pos);
    float v[3];
    getVel(v);
    CHECK(Near(pos[0], 0) && Near(pos[1], 0) && Near(pos[2], 0.2f), "fly pos (%.3f,%.3f,%.3f) want z=0.2", pos[0],
          pos[1], pos[2]);
    CHECK(Near(v[0], 0) && Near(v[1], 0) && Near(v[2], 0), "fly did not zero velocity (%.1f,%.1f,%.1f)", v[0], v[1], v[2]);

    // Live config reload: turn speed off, gravity to zero (cancels world gravity: +9.81 up).
    setPrimary(0);
    setPlayer(1.0f, 1.0f);
    WriteConfig(cfg, "speed=0\nfly=0\ngravity=zero\n# reloaded\n");
    sleep(2);  // reload check runs about once per second
    engineUpdate();
    getPlayer(p);
    lastForce(f);
    CHECK(Near(p[0], 1.0f) && Near(p[1], 1.0f), "speed=0 still modifying player (%.2f/%.2f)", p[0], p[1]);
    CHECK(Near(f[1], 9.81f), "zero gravity force y=%.2f want 9.81", f[1]);

    // Second Poison_Start() is a no-op: no second managed invoke.
    start();
    sleep(2);
    CHECK(bootstraps() == 1, "Bootstrap invoked again after second Poison_Start() (%d)", bootstraps());

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS (frames=%d bootstraps=%d)\n", frames, bootstraps());
    return 0;
}
