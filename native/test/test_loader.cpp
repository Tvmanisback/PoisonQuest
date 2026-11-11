// Host test: libPoison must wait for libil2cpp.so, invoke the managed entry once, hook the
// per-frame update, and apply every native mod from the config file.
#include <dlfcn.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        ++g_checks;                                           \
        if (!(cond)) {                                        \
            std::fprintf(stderr, "FAIL line %d: ", __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                \
            std::fputc('\n', stderr);                         \
            ++g_failures;                                     \
        }                                                     \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
static bool NearV(const float* v, float x, float y, float z) { return Near(v[0], x) && Near(v[1], y) && Near(v[2], z); }

static const char* g_cfg = nullptr;

static void WriteConfig(const std::string& text) {
    FILE* f = std::fopen(g_cfg, "w");
    std::fputs(text.c_str(), f);
    std::fclose(f);
    usleep(200 * 1000);  // POISON_RELOAD_MS=50 in this test; leave margin
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

struct Fake {
    int (*bootstraps)();
    int (*updates)();
    void (*engineUpdate)();
    void (*setPrimary)(int);
    void (*setGrab)(int);
    void (*setTrigger)(float);
    void (*setJoystick)(float, float);
    void (*setHeadAxes)(float, float, float, float, float, float);
    void (*setBodyAxes)(float, float, float, float, float, float);
    void (*setTime)(float);
    void (*setGrounded)(int);
    void (*setPlayer)(float, float);
    void (*getPlayer)(float*);
    void (*getPos)(float*);
    void (*setPos)(float, float, float);
    void (*getVel)(float*);
    void (*setVel)(float, float, float);
    int (*forceCalls)();
    int (*lastMode)();
    void (*lastForce)(float*);
};

// Resets the simulated game to a neutral state before each case.
static void Reset(Fake& F) {
    F.setPrimary(0);
    F.setGrab(0);
    F.setTrigger(0.0f);
    F.setJoystick(0, 0);
    F.setGrounded(0);
    F.setHeadAxes(0, 0, 1, 1, 0, 0);
    F.setBodyAxes(0, 0, 1, 1, 0, 0);
    F.setPos(0, 0, 0);
    F.setVel(0, 0, 0);
    F.setPlayer(6.5f, 1.1f);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: test_loader <libPoison.so> <libil2cpp.so> <config path>\n");
        return 2;
    }
    g_cfg = argv[3];
    FILE* init = std::fopen(g_cfg, "w");
    std::fputs("speed=1\nmaxspeed=9\njumpmulti=2\nfly=1\nflyspeed=10\ngravity=low\n", init);
    std::fclose(init);
    setenv("POISON_CONFIG", g_cfg, 1);
    setenv("POISON_RELOAD_MS", "50", 1);

    // Load the loader first: il2cpp is not mapped yet, so it has to wait.
    void* poison = dlopen(argv[1], RTLD_NOW);
    if (!poison) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    auto version = Sym<const char* (*)()>(poison, "Poison_Version");
    auto start = Sym<void (*)()>(poison, "Poison_Start");
    std::printf("loaded %s\n", version());

    sleep(1);
    void* il = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!il) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }

    Fake F{};
    F.bootstraps = Sym<int (*)()>(il, "fake_bootstrap_count");
    F.updates = Sym<int (*)()>(il, "fake_update_count");
    F.engineUpdate = Sym<void (*)()>(il, "fake_engine_update");
    F.setPrimary = Sym<void (*)(int)>(il, "fake_set_primary");
    F.setGrab = Sym<void (*)(int)>(il, "fake_set_grab");
    F.setTrigger = Sym<void (*)(float)>(il, "fake_set_trigger");
    F.setJoystick = Sym<void (*)(float, float)>(il, "fake_set_joystick");
    F.setHeadAxes = Sym<void (*)(float, float, float, float, float, float)>(il, "fake_set_head_axes");
    F.setBodyAxes = Sym<void (*)(float, float, float, float, float, float)>(il, "fake_set_body_axes");
    F.setTime = Sym<void (*)(float)>(il, "fake_set_time");
    F.setGrounded = Sym<void (*)(int)>(il, "fake_set_grounded");
    F.setPlayer = Sym<void (*)(float, float)>(il, "fake_set_player_state");
    F.getPlayer = Sym<void (*)(float*)>(il, "fake_get_player_state");
    F.getPos = Sym<void (*)(float*)>(il, "fake_get_position");
    F.setPos = Sym<void (*)(float, float, float)>(il, "fake_set_position");
    F.getVel = Sym<void (*)(float*)>(il, "fake_get_velocity");
    F.setVel = Sym<void (*)(float, float, float)>(il, "fake_set_velocity");
    F.forceCalls = Sym<int (*)()>(il, "fake_force_calls");
    F.lastMode = Sym<int (*)()>(il, "fake_last_mode");
    F.lastForce = Sym<void (*)(float*)>(il, "fake_get_last_force");

    // ---- Baseline: loader + original three mods ------------------------------------
    float p[2] = {0, 0};
    int frames = 0;
    for (int i = 0; i < 40 && !Near(p[0], 9.0f); ++i) {
        F.engineUpdate();
        ++frames;
        F.getPlayer(p);
        if (!Near(p[0], 9.0f)) sleep(1);
    }
    CHECK(F.bootstraps() == 1, "managed Bootstrap invoked %d times (want 1)", F.bootstraps());
    CHECK(Near(p[0], 9.0f) && Near(p[1], 2.0f), "speed boost: got %.2f/%.2f want 9/2", p[0], p[1]);
    CHECK(F.updates() == frames, "original LateUpdate ran %d times for %d frames", F.updates(), frames);

    float f[3], v[3], pos[3];
    F.lastForce(f);
    CHECK(F.forceCalls() >= 1 && NearV(f, 0, 6.66f, 0) && F.lastMode() == 5, "low gravity force (%.2f,%.2f,%.2f)", f[0],
          f[1], f[2]);

    Reset(F);
    F.setVel(5, 5, 5);
    F.engineUpdate();
    F.getPos(pos);
    CHECK(Near(pos[2], 0.0f), "fly moved without button: z=%.3f", pos[2]);
    F.setPrimary(1);
    F.engineUpdate();
    F.getPos(pos);
    F.getVel(v);
    CHECK(NearV(pos, 0, 0, 0.2f), "fly pos (%.3f,%.3f,%.3f) want z=0.2", pos[0], pos[1], pos[2]);
    CHECK(NearV(v, 0, 0, 0), "fly did not zero velocity");

    WriteConfig("speed=0\nfly=0\ngravity=zero\n# reloaded\n");
    Reset(F);
    F.setPlayer(1.0f, 1.0f);
    F.engineUpdate();
    F.getPlayer(p);
    F.lastForce(f);
    CHECK(Near(p[0], 1.0f) && Near(p[1], 1.0f), "speed=0 still modifying player (%.2f/%.2f)", p[0], p[1]);
    CHECK(Near(f[1], 9.81f), "zero gravity force y=%.2f want 9.81", f[1]);

    // ---- Batch 2 -------------------------------------------------------------------
    // TriggerFly: hold -> head forward * flyspeed, release -> zero once.
    WriteConfig("triggerfly=1\nflyspeed=10\n");
    Reset(F);
    F.setTrigger(0.8f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, 10), "triggerfly held (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.setTrigger(0.0f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, 0), "triggerfly release did not zero velocity");

    // JoystickFly: stick forward/right, normalized, then release.
    WriteConfig("joystickfly=1\nflyspeed=10\n");
    Reset(F);
    F.setJoystick(0.0f, 1.0f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, 10), "joystickfly forward (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.setJoystick(1.0f, 0.0f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 10, 0, 0), "joystickfly right (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.setJoystick(0.0f, 0.0f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, 0), "joystickfly release did not zero velocity");

    // Dash: only on the rising edge.
    WriteConfig("dash=1\nflyspeed=10\n");
    Reset(F);
    F.engineUpdate();  // primary up: records previous=false
    F.setPrimary(1);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, 10), "dash edge (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, 10), "dash fired again while held (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);

    // ReverseVelocity: flips once, then waits out the 0.5 s cooldown.
    WriteConfig("reversevelocity=1\n");
    Reset(F);
    F.setTime(200.0f);
    F.setVel(1, 2, 3);
    F.setPrimary(1);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, -1, -2, -3), "reverse (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, -1, -2, -3), "reverse fired during cooldown");
    F.setTime(200.6f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 1, 2, 3), "reverse after cooldown (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);

    // UpAndDown: trigger up, grip down (0.02 * 10 * 3 = 0.6), cancels gravity.
    WriteConfig("updown=1\nflyspeed=10\n");
    Reset(F);
    F.setTrigger(1.0f);
    F.engineUpdate();
    F.getVel(v);
    F.lastForce(f);
    CHECK(NearV(v, 0, 0.6f, 0), "updown trigger (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    CHECK(Near(f[1], 9.81f), "updown did not cancel gravity (%.2f)", f[1]);
    F.setTrigger(0.0f);
    F.setGrab(1);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0.0f, 0), "updown grip (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);

    // LeftAndRight on the body's right axis: trigger = left (-), grip = right (+).
    WriteConfig("leftright=1\nflyspeed=10\n");
    Reset(F);
    F.setBodyAxes(0, 0, 1, 1, 0, 0);
    F.setTrigger(1.0f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, -0.6f, 0, 0), "leftright trigger (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.setTrigger(0.0f);
    F.setGrab(1);
    F.setVel(0, 0, 0);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0.6f, 0, 0), "leftright grip (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);

    // ForwardsAndBackwards on the body's forward axis.
    WriteConfig("forwardback=1\nflyspeed=10\n");
    Reset(F);
    F.setBodyAxes(0, 0, 1, 1, 0, 0);
    F.setTrigger(1.0f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, 0.6f), "forwardback trigger (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.setTrigger(0.0f);
    F.setGrab(1);
    F.setVel(0, 0, 0);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 0, -0.6f), "forwardback grip (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);

    // FeatherFalling: airborne damping, then ground clamp of fall speed to -2.
    WriteConfig("featherfalling=1\n");
    Reset(F);
    F.setVel(0, -5, 0);
    F.setGrounded(0);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, -4.6f, 0), "feather airborne (%.3f,%.3f,%.3f)", v[0], v[1], v[2]);
    F.setVel(0, -5, 0);
    F.setGrounded(1);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, -2.0f, 0), "feather grounded clamp (%.3f,%.3f,%.3f)", v[0], v[1], v[2]);

    // FunMove: velocity grows by velocity * dt.
    WriteConfig("funmove=1\n");
    Reset(F);
    F.setVel(1, 0, 0);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 1.02f, 0, 0), "funmove (%.3f,%.3f,%.3f)", v[0], v[1], v[2]);

    // Strafe: horizontal velocity = body forward * maxJumpSpeed, vertical kept.
    WriteConfig("strafe=1\n");
    Reset(F);
    F.setVel(0, 3, 0);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 3, 6.5f), "strafe (%.3f,%.3f,%.3f)", v[0], v[1], v[2]);

    // DynamicStrafe: keeps horizontal speed (|(3,4)| = 5), steers it along body forward.
    WriteConfig("dynamicstrafe=1\n");
    Reset(F);
    F.setVel(3, 1, 4);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 0, 1, 5), "dynamicstrafe (%.3f,%.3f,%.3f)", v[0], v[1], v[2]);

    // GroundHelper: grip cancels upward velocity only.
    WriteConfig("groundhelper=1\n");
    Reset(F);
    F.setGrab(1);
    F.setVel(1, 2, 3);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 1, 0, 3), "groundhelper up (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    F.setVel(1, -2, 3);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 1, -2, 3), "groundhelper touched downward velocity");

    // Everything off: no changes at all.
    WriteConfig("gravity=none\n");
    Reset(F);
    F.setVel(4, 4, 4);
    F.setPrimary(1);
    F.setGrab(1);
    F.setTrigger(1.0f);
    F.engineUpdate();
    F.getVel(v);
    CHECK(NearV(v, 4, 4, 4), "mods ran with everything disabled");

    // Second Poison_Start() is a no-op: no second managed invoke.
    start();
    sleep(2);
    CHECK(F.bootstraps() == 1, "Bootstrap invoked again after second Poison_Start() (%d)", F.bootstraps());

    if (g_failures) {
        std::fprintf(stderr, "%d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS (%d checks, frames=%d)\n", g_checks, frames);
    return 0;
}
