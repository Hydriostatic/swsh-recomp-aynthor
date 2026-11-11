// Android host layer (runtime/platform.h): AArch64 handhelds (made for the AYN Thor: Snapdragon 8 Gen 2 / Adreno 740).
// From runtime/platform/linux.c; what differs:
//  - the app is a library inside an SDL activity (android/): SDL's Java thread calls SDL_main below, which sets up the
//    folders and runs the runtime's own main() on a thread with a stack large enough for the game code;
//  - no child processes (no compiler on the device: the game code library is built on a PC / in CI,
//    docs/ANDROID-THOR.md) and no re-exec;
//  - the data root is the app's folder on shared storage (Android/data/<package>/files), where the player puts the
//    game files; libraries can't be loaded from there, so the game code is staged in the app's private folder first.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include "../paths.h"
#include "../platform.h"

#define TAG "SwShRecomp"
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// As on Linux: everything readable/writable from the start, untouched pages cost nothing. The window itself is not
// where the desktop builds have it: an app process is forked from the zygote, whose libraries and heaps sit at random
// addresses in the top 64 GB of the 39-bit address space - right where 0x70_0000_0000 is - and a fork can't re-roll
// that layout by re-executing itself. The Android build therefore moves every guest address down to 0x10_0000_0000,
// far below anything the system maps (tools/android/rebase_guest.py rewrites the sources at build time).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        ALOG("guest window %llx-%llx: mmap failed (%s)", (unsigned long long)lo, (unsigned long long)hi, strerror(errno));
        return false;
    }
    if (p != (void *)(uintptr_t)lo) {
        ALOG("guest window %llx-%llx is taken (got %p)", (unsigned long long)lo, (unsigned long long)hi, p);
        munmap(p, hi - lo);
        return false;
    }
    return true;
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

void *plat_reserve_lazy(uint64_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

long plat_written_pages(uint64_t addr, uint64_t size, uint64_t *out, long max) { (void)addr; (void)size; (void)out; (void)max; return -1; }

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// (an app can raise its threads to -20 at most where the system lets it; failing is harmless)
void plat_thread_realtime(uint64_t period_ns) { (void)period_ns; setpriority(PRIO_PROCESS, (id_t)gettid(), -10); }
void plat_thread_background(void) { setpriority(PRIO_PROCESS, (id_t)gettid(), 5); }
void plat_thread_idle(void) { setpriority(PRIO_PROCESS, (id_t)gettid(), 19); }

// ---- folders
static char g_data[900];      // shared storage: game files, saves, caches, the log (the data root, runtime/paths.h)
static char g_private[900];   // the app's own folder: shipped resources (from the APK's assets) and the staged game code
static char g_log_path[1024];

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return false; }
    static char buf[1 << 20];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    bool ok = true;
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    pthread_mutex_unlock(&lock);
    ok &= !ferror(in);
    fclose(in);
    ok &= fclose(out) == 0;
    if (ok) ok = rename(tmp, dst) == 0;
    if (!ok) remove(tmp);
    return ok;
}

// A file shipped in the APK's assets, copied to the private folder (SDL reads relative paths from the assets).
static void unpack_asset(const char *name) {
    char dst[1100];
    snprintf(dst, sizeof dst, "%s/res/%s", g_private, name);
    size_t size = 0;
    void *data = SDL_LoadFile(name, &size);
    if (!data) { ALOG("asset %s: %s", name, SDL_GetError()); return; }
    struct stat st;
    if (stat(dst, &st) != 0 || (size_t)st.st_size != size) {
        FILE *f = fopen(dst, "wb");
        if (f) { fwrite(data, 1, size, f); fclose(f); }
    }
    SDL_free(data);
}

// The game code library can't be loaded from shared storage (mounted noexec, and outside the paths the linker
// allows an app): it is copied to the private folder when it changed (size / time), and loaded from there
// (runtime/gamelib.c). out: the path to load.
bool plat_android_stage_lib(const char *src, char *out, size_t n) {
    struct stat st;
    if (stat(src, &st) != 0) return false;
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char dir[1000], stamp_path[1100], stamp[128], have[128] = {0};
    snprintf(dir, sizeof dir, "%s/lib", g_private);
    mkdirs(dir);
    snprintf(out, n, "%s/%s", dir, base);
    snprintf(stamp_path, sizeof stamp_path, "%s.stamp", out);
    snprintf(stamp, sizeof stamp, "%lld %lld", (long long)st.st_size, (long long)st.st_mtime);
    FILE *f = fopen(stamp_path, "rb");
    if (f) { size_t k = fread(have, 1, sizeof have - 1, f); have[k] = 0; fclose(f); }
    struct stat cur;
    if (!strcmp(have, stamp) && stat(out, &cur) == 0 && cur.st_size == st.st_size) return true;
    fprintf(stderr, "[android] staging %s (%lld MB) in the app's folder\n", src, (long long)(st.st_size >> 20));
    remove(stamp_path);
    if (!copy_file(src, out)) { fprintf(stderr, "[android] cannot copy %s to %s: %s\n", src, out, strerror(errno)); return false; }
    chmod(out, 0700);
    f = fopen(stamp_path, "wb");
    if (f) { fputs(stamp, f); fclose(f); }
    return true;
}

const char *plat_android_data_dir(void) { return g_data; }

// ---- process
// stderr / stdout go to lasrecomp.log in the data root (reachable over USB), the previous run's kept for crash
// reports as on the other platforms; the first lines also go to logcat (tag SwShRecomp).
static void log_to_file(void) {
    snprintf(g_log_path, sizeof g_log_path, "%s/lasrecomp.log", g_data);
    char prev[1024];
    snprintf(prev, sizeof prev, "%s/lasrecomp-prev.log", g_data);
    rename(g_log_path, prev);
    if (!freopen(g_log_path, "w", stderr)) { ALOG("cannot open %s: %s", g_log_path, strerror(errno)); g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    ALOG("log: %s", g_log_path);
}

void plat_init(void) {}  // (SDL_main below has done it: the folders have to exist before main() starts)

// "The executable's folder" (runtime/paths.c: where shipped resources are looked up): the unpacked assets.
bool plat_exe_path(char *buf, size_t size) {
    return (size_t)snprintf(buf, size, "%s/res/SwShRecomp", g_private) < size;
}

void plat_exec(char *const argv[]) { (void)argv; errno = ENOSYS; }  // (an activity can't replace its process)

void plat_terminate(void) { _exit(0); }

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

void plat_fatal_notice(void) {
    fflush(stderr);
    char text[1200];
    snprintf(text, sizeof text, RT_APP_NAME " stopped because of an error.\n\nDetails: %s", g_log_path[0] ? g_log_path : "logcat");
    ALOG("%s", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, RT_APP_NAME, text, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

typedef struct { void **pcs; int n, max; } Unwind;
static _Unwind_Reason_Code unwind_step(struct _Unwind_Context *ctx, void *arg) {
    Unwind *u = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && u->n < u->max) u->pcs[u->n++] = (void *)pc;
    return u->n < u->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

// (bionic has no backtrace() before API 33: the unwinder itself)
void plat_print_backtrace(void) {
    void *bt[64];
    Unwind u = {bt, 0, 64};
    _Unwind_Backtrace(unwind_step, &u);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < u.n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL, di.dli_fbase = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s+%llx)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod,
                       (unsigned long long)moff);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    ALOG("%s at %llx (pc %llx): see %s", what, (unsigned long long)fa, (unsigned long long)pc, g_log_path);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes: none on Android (the callers report the failure: runtime/gamebuild.c, runtime/mods.c)
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)log_path, (void)append_log;
    fprintf(stderr, "[android] not started (no child processes here): %s\n", argv && argv[0] ? argv[0] : "?");
    return 0;
}
void plat_proc_kill(PlatProc p) { (void)p; }
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return 255; }

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// ---- entry
// The window's surface goes away while the app is in the background and a new one comes with the return
// (runtime/nvn/vulkan/device.c waits and then makes its swapchain again). Called from the activity's thread.
static _Atomic int g_background, g_surface_gen;
bool plat_android_in_background(void) { return g_background; }
int plat_android_surface_generation(void) { return g_surface_gen; }

static bool lifecycle_watch(void *user, SDL_Event *e) {
    (void)user;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) g_background = 1;
    else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) { g_surface_gen++; g_background = 0; }
    return true;
}

int main(int argc, char **argv);  // runtime/main.c

typedef struct { int argc; char **argv; int rc; } MainArgs;
static void *main_thread(void *arg) {
    MainArgs *a = arg;
    a->rc = main(a->argc, a->argv);
    return NULL;
}

// Called by SDL's activity on its own thread ("SDLThread", a 1 MB stack): the game's main thread runs recompiled
// code that recurses on the host stack (the other guest threads get 256 MB, runtime/kernel.c), so main() gets a
// thread of its own with the same.
__attribute__((visibility("default"))) int SDL_main(int argc, char *argv[]) {
    const char *ext = SDL_GetAndroidExternalStoragePath(), *priv = SDL_GetAndroidInternalStoragePath();
    if (!priv) { ALOG("no storage path: %s", SDL_GetError()); return 1; }
    snprintf(g_private, sizeof g_private, "%s", priv);
    snprintf(g_data, sizeof g_data, "%s", ext && *ext ? ext : priv);  // (no shared storage: everything private)
    mkdirs(g_data);
    char p[1000];
    snprintf(p, sizeof p, "%s/res", g_private);
    mkdirs(p);
    snprintf(p, sizeof p, "%s/extracted", g_data);  // where the game files go (docs/ANDROID-THOR.md)
    mkdirs(p);
    log_to_file();
    setenv("LARECOMP_DATA_ROOT", g_data, 1);
    setenv("HOME", g_private, 1);
    const char *cache = SDL_GetAndroidCachePath();
    if (cache) setenv("TMPDIR", cache, 1);
    // settings for the device, unless larecomp.env in the data root says otherwise: one NAME=VALUE per line (the
    // LARECOMP_* variables of docs/DEPURACAO.md; there is no command line to set them from here)
    snprintf(p, sizeof p, "%s/larecomp.env", g_data);
    FILE *f = fopen(p, "rb");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq || eq == line) continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[android] %s=%s\n", line, eq + 1);
        }
        fclose(f);
    }
    unpack_asset("Baloo2-SemiBold.ttf");
    unpack_asset("icon.png");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // Back (a handheld's controller often sends it for Select / its back button) is a key for the game and the
    // menus, not "close the app"; the built-in controls count as a gamepad even while nothing else is plugged in
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_AddEventWatch(lifecycle_watch, NULL);

    MainArgs a = {argc, argv, 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256ull << 20);
    pthread_t t;
    if (pthread_create(&t, &attr, main_thread, &a) != 0) { ALOG("cannot start the main thread: %s", strerror(errno)); return 1; }
    pthread_join(t, NULL);
    return a.rc;
}

// ---- second screen (android/.../DualScreen.java: the AYN Thor's bottom screen asks a few times a second)
#include <jni.h>

#include "../panel.h"

void launcher_android_folder_picked(const char *path);  // runtime/ui/launcher_lg.mm

void plat_android_pick_folder(void) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) { fprintf(stderr, "[android] no activity for the folder picker\n"); return; }
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, "pickGameFolder", "()V") : NULL;
    if (m) (*env)->CallVoidMethod(env, act, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}

// MainActivity: the picked folder's path, or NULL when the picker was closed
__attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_io_github_hydriostatic_swshrecomp_MainActivity_nativeFolderPicked(JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    if (!path) return;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    if (!p) return;
    fprintf(stderr, "[android] game folder: %s\n", p);
    launcher_android_folder_picked(p);
    (*env)->ReleaseStringUTFChars(env, path, p);
}

__attribute__((visibility("default"))) JNIEXPORT jstring JNICALL
Java_io_github_hydriostatic_swshrecomp_DualScreen_nativeSnapshot(JNIEnv *env, jclass cls) {
    (void)cls;
    static char buf[16384];  // (one caller: the panel's UI thread)
    panel_snapshot(buf, sizeof buf);
    return (*env)->NewStringUTF(env, buf);
}
