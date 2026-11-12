// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#include <SDL3/SDL_cpuinfo.h>
#ifdef __ANDROID__
#include "romfs_build.h"
#endif

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(char **argv, const char *log) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p) {
    int r;
    while ((r = finished(p)) < 0) sleep_ms(20);
    return r;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: lasgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or LARECOMP_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

#ifdef __ANDROID__
// Android: python and zig installed with the APK (its library folder: the only place an app may run programs
// from), their files unpacked from assets/genkit.zip by MainActivity (<private>/res, done when res/genkit.ok
// exists). tools/android/build_genkit.sh makes all of it.
static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    const char *lib = plat_android_lib_dir(), *priv = plat_android_private_dir();
    char ok[1100];
    snprintf(ok, sizeof ok, "%s/res/genkit.ok", priv);
    for (int i = 0; i < 600 && !file_exists(ok); i++) {  // (still unpacking right after the first start: up to 2 min)
        if (i == 0) LOG("[gamebuild] waiting for the builder's files to be unpacked\n");
        plat_sleep_until_ns(plat_now_ns() + 200000000ull);
    }
    snprintf(t->gen, sizeof t->gen, "%s/libpyexe.so", lib);
    snprintf(t->gen_script, sizeof t->gen_script, "%s/res/tools/genpar.py", priv);  // (gen.py on several cores)
    snprintf(t->cc, sizeof t->cc, "%s/libzig.so", lib);
    t->zig = true;
    snprintf(t->inc, sizeof t->inc, "%s/res/gamelib-src", priv);
    snprintf(t->entry, sizeof t->entry, "%s/res/gamelib-src/gamelib_entry.c", priv);
    snprintf(t->hle, sizeof t->hle, "%s/res/gamelib-src/hle_symbols.txt", priv);
    bool have = file_exists(ok) && file_exists(t->gen) && file_exists(t->cc) && file_exists(t->gen_script) && file_exists(t->entry);
    if (!have) LOG("[gamebuild] builder missing: %s %s %s\n", t->gen, t->cc, ok);
    return have;
}
#else
static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    const char *e = getenv("LARECOMP_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("lasgen" SEP "lasgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "." PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&  // (a source checkout: .venv)
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("LARECOMP_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource("lasrecomp.dll.a", "build/lasrecomp.dll.a", t->implib, sizeof t->implib)) return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}
#endif

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    if (t->zig) add(a, "cc");
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], log[1100];
    unsigned long long bytes;
    int tries;          // (Android: 1 = compiled again with zig cc after the direct clang failed)
    char key[40];       // (Android: its object cache entry, "" = not cached)
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
    u->tries = 0;
    u->key[0] = 0;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
static void add_cflags(Args *a, const Tools *t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
}

#ifdef __ANDROID__
// The clang inside zig, called directly ("zig clang"): no zig cc in between, which hashes every source and header
// into its cache and runs clang as one more process per file. The include folders and target are what zig cc
// passes (res/zig/clang_flags.txt, tools/android/build_genkit.sh, checked there); a file this fails on is compiled
// again with zig cc (start_compile, tries 1).
static void add_direct_clang(Args *a, const Tools *t) {
    add(a, t->cc);
    add(a, "clang");
    char path[1100], line[1200];
    snprintf(path, sizeof path, "%s/res/zig/clang_flags.txt", plat_android_private_dir());
    FILE *f = fopen(path, "rb");
    while (f && fgets(line, sizeof line, f)) {  // (one flag a line; "@ZIG@" = the zig lib folder)
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char *at = strstr(line, "@ZIG@"), buf[1400];
        if (at) {
            *at = 0;
            snprintf(buf, sizeof buf, "%s%s/res/zig/lib%s", line, plat_android_private_dir(), at + 5);
            add(a, buf);
        } else {
            add(a, line);
        }
    }
    if (f) fclose(f);
    char flags[1024];
    snprintf(flags, sizeof flags, "%s", GAMELIB_CFLAGS);
    bool skip = false;
    for (char *x = strtok(flags, " "); x; x = strtok(NULL, " ")) {  // (zig's own target spelling: given above)
        if (skip) { skip = false; continue; }
        if (!strcmp(x, "-target")) { skip = true; continue; }
        add(a, x);
    }
}
static bool direct_clang_ok(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/res/zig/clang_flags.txt", plat_android_private_dir());
    return file_exists(path) && !getenv("LARECOMP_ZIG_CC");
}
#endif

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
#ifdef __ANDROID__
    if (u->tries == 0 && direct_clang_ok()) {
        add_direct_clang(&a, t);
        char inc[1200];
        snprintf(inc, sizeof inc, "-I%s", t->inc);
        add(&a, inc);
        add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
        add(&a, "-c");
        add(&a, u->src);
        add(&a, "-o");
        add(&a, u->obj);
        PlatProc p = spawn(a.v, u->log);
        args_free(&a);
        return p;
    }
#endif
    compiler(&a, t);
    add_cflags(&a, t);
#ifdef __ANDROID__
    // zig cc turns -O1 into ReleaseFast, which passes -O2 to clang: twice the compile time of the -O1 the game code
    // is meant to be built with (upstream: 40 minutes with zig, 20 with clang). The last -O clang sees decides.
    add(&a, "-Xclang");
    add(&a, "-O1");
#endif
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->obj);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

#ifdef __ANDROID__
// ------------------------------------------------------------------ object cache (Android)
// Every compiled file kept in the app's private storage under a key of its C, the game code id (generator,
// headers, flags) and the builder's version: a build that stopped half way (the app closed, the battery) resumes
// with what it had compiled, and a rebuild after an update compiles only the files whose C changed (tools/gen.py
// cuts the files where the code itself says, so a change stays in its own files). Only the last build's objects are
// kept.
static void cache_dir(char *out, size_t n) { snprintf(out, n, "%s/objcache", plat_android_private_dir()); }

static bool unit_key(const Unit *u, char out[40]) {
    FILE *f = fopen(u->src, "rb");
    if (!f) return false;
    static char salt[200];
    if (!salt[0]) {
        char v[1100], ver[128] = "";
        snprintf(v, sizeof v, "%s/res/genkit.ok", plat_android_private_dir());
        FILE *g = fopen(v, "rb");
        if (g) { size_t k = fread(ver, 1, sizeof ver - 1, g); ver[k] = 0; fclose(g); }
        snprintf(salt, sizeof salt, "%s|%s|%s", GAMELIB_ID, GAMELIB_CFLAGS, ver);
    }
    uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x84222325cbf29ce4ull;  // (FNV-1a, and a second pass mixed differently)
    for (const char *s = salt; *s; s++) { h1 = (h1 ^ (uint8_t)*s) * 0x100000001b3ull; h2 = (h2 + (uint8_t)*s) * 0x9e3779b97f4a7c15ull; }
    static unsigned char buf[1 << 16];
    unsigned long long len = 0;
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) {
        for (size_t i = 0; i < k; i++) {
            h1 = (h1 ^ buf[i]) * 0x100000001b3ull;
            h2 = (h2 ^ (buf[i] + (h2 >> 29))) * 0xff51afd7ed558ccdull;
        }
        len += k;
    }
    fclose(f);
    snprintf(out, 40, "%016llx%016llx", (unsigned long long)h1, (unsigned long long)(h2 ^ len));
    return true;
}

static bool cache_take(Unit *u) {  // the object of the same C: hard-linked into place (the same storage)
    if (!unit_key(u, u->key)) { u->key[0] = 0; return false; }
    char dir[1100], p[1200];
    cache_dir(dir, sizeof dir);
    snprintf(p, sizeof p, "%s/%s.o", dir, u->key);
    struct stat st;
    if (stat(p, &st) != 0) return false;
    remove(u->obj);
    if (link(p, u->obj) != 0) return false;
    utimensat(AT_FDCWD, p, NULL, 0);  // (used by this build: kept)
    return true;
}

static void cache_put(const Unit *u) {
    if (!u->key[0]) return;
    char dir[1100], p[1200];
    cache_dir(dir, sizeof dir);
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/%s.o", dir, u->key);
    remove(p);
    link(u->obj, p);
}

static void cache_prune(uint64_t since_ns) {  // (after a complete build: what it didn't use goes)
    char dir[1100];
    cache_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    int n = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec < since_ns) {
            remove(p);
            n++;
        }
    }
    closedir(d);
    if (n) LOG("[gamebuild] object cache: %d old objects removed\n", n);
}

// Free memory, MB (MemAvailable): another compiler starts only while there is room for it - a handheld shares its
// RAM with the system, and Android ends an app that takes too much
static long mem_available_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    long kb = -1;
    char line[128];
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb < 0 ? 1 << 20 : kb / 1024;
}
#endif

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of " RT_APP_NAME);
        return false;
    }
    char work[1100], lib[1100], tmp[1100];
#ifdef __ANDROID__
    // the generated C and the objects (a thousand files, over 1 GB) in the app's private storage: shared storage
    // (Download...) goes through Android's slower FUSE layer
    snprintf(work, sizeof work, "%s/gamebuild", plat_android_private_dir());
    plat_remove_tree(work);
#else
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
#endif
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    rt_remove_game_file(game_dir, ".gamebuild");
    rt_remove_game_file(game_dir, GAMELIB_NAME);
    rt_remove_game_file(game_dir, GAMELIB_NAME ".id");
    mkdir(work, 0755);
    if (t.zig) {  // its caches in the work folder too (deleted below), not ~/.cache: hundreds of MB, used once
        char zc[1200];
#ifdef __ANDROID__
        // (the app's private storage: shared storage - Download and the like - has no file locks, which zig's cache
        //  needs: "checking cache failed: manifest_create Unexpected")
        snprintf(zc, sizeof zc, "%s/zig-cache", plat_android_private_dir());
        plat_remove_tree(zc);
#else
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
#endif
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);

    // 1. the generator, every module at once; their finished files are compiled while it goes on (bases: rt.h)
    enum { NGEN = 4 };
    Gen gens[NGEN] = {{.name = "main"}, {.name = "sdk"}, {.name = "subsdk0"}, {.name = "subsdk1"}};
    static const char *const base[NGEN] = {"0x7000000000", "0x7004000000", "0x7006000000", "0x7008000000"};
    bool ok = true;
    for (int i = 0; i < NGEN && ok; i++) {
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, g->name);
        add(&a, nso);
        add(&a, base[i]);
        add(&a, g->out);
        if (i == 1) { add(&a, "--hle"); add(&a, t.hle); }
#ifdef __ANDROID__
        {   // tools/android/genpar.py: main is most of the work (every core), sdk half, the two small ones two each
            char jobs[16];
            int cores = SDL_GetNumLogicalCPUCores();
            snprintf(jobs, sizeof jobs, "%d", i == 0 ? cores : i == 1 ? (cores / 2 > 1 ? cores / 2 : 1) : 2);
            add(&a, "--jobs");
            add(&a, jobs);
        }
#endif
        g->p = spawn(a.v, g->log);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    Units units = {0};
    int cache_hits = 0;
#ifdef __ANDROID__
    struct timespec rts;
    clock_gettime(CLOCK_REALTIME, &rts);
    uint64_t t_real0 = (uint64_t)rts.tv_sec * 1000000000ull + (uint64_t)rts.tv_nsec - 5000000000ull;  // (file times)
#endif
    (void)cache_hits;
    int queued[NGEN] = {0};  // chunk files of each module put in units so far
    bool tables[NGEN] = {false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
    }
    while (ok) {
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < NGEN; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed: %s\n", g->name, last);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            gen_frac += (g->exit == 0 ? 1 : g->n ? (double)g->ci / (double)g->n : 0) / NGEN;  // (0..1 over all modules)
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles
        while (nrun < jobs && next < units.n) {
#ifdef __ANDROID__
            if (cache_take(&units.u[next])) {  // (compiled before: the same C)
                done_bytes += units.u[next].bytes;
                cache_hits++;
                if (next > 0) remove(units.u[next].src);
                next++;
                continue;
            }
            // room for one more compiler (about 1.5 GB at worst), else wait for one to finish
            if (nrun > 0 && mem_available_mb() < 2200) break;
#endif
            PlatProc p = start_compile(&t, &units.u[next]);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
#ifdef __ANDROID__
            if (r != 0 && u->tries == 0 && direct_clang_ok()) {  // (the direct clang failed: once more with zig cc)
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] %s: direct clang failed (%s), with zig cc\n", u->src, last);
                u->tries = 1;
                PlatProc p = start_compile(&t, u);
                if (p) { run[k].p = p; continue; }
            }
            if (r == 0) cache_put(u);
#endif
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed: %s\n", u->src, last);
                snprintf(why, why_size, "the game code didn't compile");
                ok = false;
            }
            done_bytes += u->bytes;
            if (run[k].unit > 0) remove(u->src);  // (generated C, done with: not kept. Unit 0 is the shipped entry file)
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) break;

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < NGEN; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 2. link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#elif defined(__ANDROID__)
        {  // the linker itself (tools/android/build_genkit.sh checks this same link): against the app's libmain.so,
           // whose functions the game code calls, and zig's runtime library built ahead of time
            char opt[1200];
            add(&a, t.cc);
            add(&a, "ld.lld");
            add(&a, "-shared");
            add(&a, "--eh-frame-hdr");  // (the unwind tables found by the unwinder: crash backtraces through the game code)
            add(&a, "-z");
            add(&a, "max-page-size=16384");
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
            snprintf(opt, sizeof opt, "%s/libmain.so", plat_android_lib_dir());
            add(&a, opt);
            snprintf(opt, sizeof opt, "%s/res/zig/rt/libcompiler_rt.a", plat_android_private_dir());
            add(&a, opt);
        }
        if (0)
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed: %s\n", last);
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        }
    }
#ifdef __ANDROID__
    if (ok) {
        LOG("[gamebuild] %d files, %d from the object cache\n", units.n, cache_hits);
        cache_prune(t_real0);
    }
#endif
    if (ok) {
        remove(lib);
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        bool moved = rename(tmp, lib) == 0;
#ifdef __ANDROID__
        if (!moved) moved = romfs_copy_file(tmp, lib, NULL);  // (private storage -> the game files folder)
#endif
        if (!moved || !(f = fopen(idp, "wb")) || fprintf(f, "%s\n", GAMELIB_ID) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
    }
    free(units.u);
    rt_remove_game_file(game_dir, ".gamebuild");  // the C, the objects, the logs
#ifdef __ANDROID__
    plat_remove_tree(work);
#endif
#ifdef __ANDROID__
    { char zc[1200]; snprintf(zc, sizeof zc, "%s/zig-cache", plat_android_private_dir()); plat_remove_tree(zc); }
#endif
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
