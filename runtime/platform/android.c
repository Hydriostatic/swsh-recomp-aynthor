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

#include "../panel.h"
#include "../paths.h"
#include "../platform.h"
#include <dirent.h>

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

static void read_line_file(const char *name, char *out, size_t n) {  // a one-line file in the private folder
    out[0] = 0;
    char p[1000];
    snprintf(p, sizeof p, "%s/%s", g_private, name);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

// ---- the player's folder for the log and the saves: Download/SwShRecomp on shared storage by default, where any file
// manager on the device reaches it, or the folder picked on the start screen (<private>/user_dir.txt). NULL when the
// app can't write there (then they stay in the data root).
static char g_user[900];
static bool writable_dir(const char *dir) {
    mkdirs(dir);
    char t[1000];
    snprintf(t, sizeof t, "%s/.write_test", dir);
    FILE *f = fopen(t, "wb");
    if (!f) return false;
    fclose(f);
    remove(t);
    return true;
}
static const char *storage_top(void) {
    const char *top = getenv("EXTERNAL_STORAGE");
    return top && *top ? top : "/storage/emulated/0";
}
const char *plat_android_user_dir(void) {
    static int checked;
    if (!checked) {
        checked = 1;
        char d[900];
        read_line_file("user_dir.txt", d, sizeof d);
        if (!d[0]) snprintf(d, sizeof d, "%s/Download/SwShRecomp", storage_top());
        if (writable_dir(d)) snprintf(g_user, sizeof g_user, "%s", d);
        else ALOG("%s is not writable: the log and saves stay in the app's folder", d);
    }
    return g_user[0] ? g_user : NULL;
}

static bool move_entry(const char *from, const char *to);  // (below)

// The start screen's "Change..." for the game files and built game code: used from the next start, when what the
// current folder has moves there. false: the folder can't be written.
bool plat_android_set_data_dir(const char *dir) {
    if (!writable_dir(dir)) return false;
    size_t n = strlen(g_data);
    if (!strcmp(dir, g_data)) return true;
    if (!strncmp(dir, g_data, n) && dir[n] == '/') return false;  // (inside the current one: it would move into itself)
    char p[1000];
    snprintf(p, sizeof p, "%s/data_prev.txt", g_private);
    FILE *f = fopen(p, "wb");
    if (f) { fprintf(f, "%s\n", g_data); fclose(f); }
    snprintf(p, sizeof p, "%s/data_dir.txt", g_private);
    f = fopen(p, "wb");
    if (f) { fprintf(f, "%s\n", dir); fclose(f); }
    fprintf(stderr, "[android] game files folder: %s from the next start\n", dir);
    return true;
}

// The start screen's "Change..." for the log and saves: moves the saves (and logs) there and uses it from now on.
// false: the folder can't be written.
bool plat_android_set_user_dir(const char *dir) {
    if (!writable_dir(dir)) return false;
    const char *old = plat_android_user_dir();
    if (old && strcmp(old, dir)) {
        DIR *d = opendir(old);
        if (d) {
            for (struct dirent *e; (e = readdir(d));) {
                if (strncmp(e->d_name, "save", 4) && strncmp(e->d_name, "lasrecomp", 9)) continue;
                char a[1100], b[1100];
                snprintf(a, sizeof a, "%s/%s", old, e->d_name);
                snprintf(b, sizeof b, "%s/%s", dir, e->d_name);
                if (!strcmp(e->d_name, "lasrecomp.log")) continue;  // (open: this run's goes on there)
                if (!move_entry(a, b)) fprintf(stderr, "[android] can't move %s to %s\n", a, b);
            }
            closedir(d);
        }
    }
    char p[1000];
    snprintf(p, sizeof p, "%s/user_dir.txt", g_private);
    FILE *f = fopen(p, "wb");
    if (f) { fprintf(f, "%s\n", dir); fclose(f); }
    snprintf(g_user, sizeof g_user, "%s", dir);
    fprintf(stderr, "[android] log and saves: %s (the log from the next start)\n", dir);
    return true;
}

// ---- process
// stderr / stdout go to lasrecomp.log in Download/SwShRecomp (or the data root) (reachable over USB), the previous run's kept for crash
// reports as on the other platforms; the first lines also go to logcat (tag SwShRecomp).
static void log_to_file(void) {
    const char *dir = plat_android_user_dir();
    if (!dir) dir = g_data;
    snprintf(g_log_path, sizeof g_log_path, "%s/lasrecomp.log", dir);
    char prev[1024];
    snprintf(prev, sizeof prev, "%s/lasrecomp-prev.log", dir);
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

// The last lines of the log (the error itself), for the dialog and the bottom screen: the log file can be hard to
// reach on the device
static void log_tail_lines(char *out, size_t n, int lines) {
    out[0] = 0;
    if (!g_log_path[0]) return;
    FILE *f = fopen(g_log_path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *p = buf + k;
    for (int i = 0; i < lines && p > buf; i++) {
        p--;
        while (p > buf && p[-1] != '\n') p--;
    }
    snprintf(out, n, "%s", p);
}

void plat_fatal_notice(void) {
    fflush(stderr);
    char tail[1200], text[1800];
    log_tail_lines(tail, sizeof tail, 8);
    char first[200];
    snprintf(first, sizeof first, "%s", tail);
    char *nl = strrchr(first, '\n');
    panel_set("error", "stopped", "%s", nl ? nl + 1 : first);
    panel_set("error", "log", "%s", g_log_path[0] ? g_log_path : "logcat");
    snprintf(text, sizeof text, RT_APP_NAME " stopped because of an error.\n\n%s\n\nLog: %s", tail[0] ? tail : "(no details)",
             g_log_path[0] ? g_log_path : "logcat");
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

// ---- child processes: the game code builder (runtime/gamebuild.c) runs python and zig, which the APK installs in
// its library folder - the one place an app may run programs from (tools/android/build_genkit.sh)
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. zig's clang)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (rc) fprintf(stderr, "[android] can't start %s: %s\n", argv[0], strerror(rc));
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

// The APK's library folder (libmain.so's: where the builder's programs are) and the unpacked builder files
// (MainActivity unpacks assets/genkit.zip into <private>/res; res/genkit.ok when done).
static char g_libdir[900];
const char *plat_android_lib_dir(void) {
    if (!g_libdir[0]) {
        Dl_info di = {0};
        if (dladdr((void *)plat_android_lib_dir, &di) && di.dli_fname) {
            snprintf(g_libdir, sizeof g_libdir, "%s", di.dli_fname);
            char *s = strrchr(g_libdir, '/');
            if (s) *s = 0;
        }
    }
    return g_libdir;
}
const char *plat_android_private_dir(void) { return g_private; }

// What the builder's programs need to find their parts (set once: children inherit it)
static void builder_env(void) {
    const char *lib = plat_android_lib_dir();
    char v[2400];
    setenv("LD_LIBRARY_PATH", lib, 1);  // (libpyexe.so -> libpython3.13.so and its libraries)
    snprintf(v, sizeof v, "%s/res/python", g_private);
    setenv("PYTHONHOME", v, 1);
    snprintf(v, sizeof v, "%s/res/pylib:%s/res/tools", g_private, g_private);
    setenv("PYTHONPATH", v, 1);
    setenv("PYTHONHASHSEED", "0", 1);  // (the same output every run, as the PC's lasgen)
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    setenv("LIBCAPSTONE_PATH", lib, 1);
    setenv("LIBUNICORN_PATH", lib, 1);
    snprintf(v, sizeof v, "%s/res/zig/lib", g_private);
    setenv("ZIG_LIB_DIR", v, 1);
}

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

// What an earlier version kept in the app's own folder moves to the data root, entry by entry, where the root doesn't
// have it yet: a rename (the same storage), else a copy.
static bool copy_tree(const char *from, const char *to) {
    struct stat st;
    if (lstat(from, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return copy_file(from, to);
    mkdir(to, 0755);
    DIR *d = opendir(from);
    if (!d) return false;
    bool ok = true;
    for (struct dirent *e; ok && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1100], b[1100];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        ok = copy_tree(a, b);
    }
    closedir(d);
    return ok;
}
static bool move_entry(const char *from, const char *to) {
    if (rename(from, to) == 0) return true;
    if (!copy_tree(from, to)) return false;
    plat_remove_tree(from);
    return true;
}
static void migrate_data(const char *from, const char *to) {
    DIR *d = opendir(from);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, "lasrecomp", 9)) continue;
        char a[1100], b[1100];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        struct stat st;
        if (stat(b, &st) == 0) continue;
        ALOG("moving %s to %s", a, b);
        if (!move_entry(a, b)) ALOG("can't move %s", a);
    }
    closedir(d);
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
    // The data root (game files, built game code, caches, settings): the folder picked on the start screen
    // (<private>/data_dir.txt), else SwShRecomp/ in internal storage - both kept when the app is reinstalled (Android
    // deletes Android/data/<package> with it). Needs all-files access (asked on the first start, MainActivity);
    // without it the app's own folder, as before. What the previous folder had moves to a newly picked one here, at
    // the start (data_prev.txt), and so does what older versions kept in the app's own folder.
    char app_dir[900], top_dir[900], chosen[900], prev[900];
    snprintf(app_dir, sizeof app_dir, "%s", ext && *ext ? ext : priv);
    snprintf(top_dir, sizeof top_dir, "%s/SwShRecomp", storage_top());
    read_line_file("data_dir.txt", chosen, sizeof chosen);
    read_line_file("data_prev.txt", prev, sizeof prev);
    if (chosen[0] && writable_dir(chosen)) snprintf(g_data, sizeof g_data, "%s", chosen);
    else if (writable_dir(top_dir)) snprintf(g_data, sizeof g_data, "%s", top_dir);
    else snprintf(g_data, sizeof g_data, "%s", app_dir);
    if (prev[0] && strcmp(prev, g_data)) migrate_data(prev, g_data);
    if (prev[0]) { char p2[1000]; snprintf(p2, sizeof p2, "%s/data_prev.txt", g_private); remove(p2); }
    if (strcmp(app_dir, g_data)) migrate_data(app_dir, g_data);
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
    builder_env();
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

void launcher_android_folder_picked(int kind, const char *path);  // runtime/ui/launcher_lg.mm
void launcher_android_update_status(const char *text);

// Calls a method of the activity (MainActivity) from any thread
static void call_activity(const char *name, const char *sig, int iarg) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) { fprintf(stderr, "[android] no activity for %s\n", name); return; }
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, name, sig) : NULL;
    if (m) (*env)->CallVoidMethod(env, act, m, iarg);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}

void plat_android_pick_folder_kind(int kind) { call_activity("pickFolder", "(I)V", kind); }
void plat_android_pick_folder(void) { plat_android_pick_folder_kind(0); }
void plat_android_update(bool install) { call_activity("updateApp", "(Z)V", install ? 1 : 0); }
void plat_android_build_status(int percent) { call_activity("buildStatus", "(I)V", percent); }

// MainActivity: the picked folder's path
__attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_io_github_hydriostatic_swshrecomp_MainActivity_nativeFolderPicked(JNIEnv *env, jclass cls, jint kind, jstring path) {
    (void)cls;
    if (!path) return;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    if (!p) return;
    fprintf(stderr, "[android] picked folder (%d): %s\n", (int)kind, p);
    launcher_android_folder_picked((int)kind, p);
    (*env)->ReleaseStringUTFChars(env, path, p);
}

// MainActivity: what the updater is doing ("" = nothing to say)
__attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_io_github_hydriostatic_swshrecomp_MainActivity_nativeUpdateStatus(JNIEnv *env, jclass cls, jstring text) {
    (void)cls;
    const char *t = text ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;
    if (t && *t) fprintf(stderr, "[update] %s\n", t);
    launcher_android_update_status(t ? t : "");
    if (t) (*env)->ReleaseStringUTFChars(env, text, t);
}

__attribute__((visibility("default"))) JNIEXPORT jstring JNICALL
Java_io_github_hydriostatic_swshrecomp_DualScreen_nativeSnapshot(JNIEnv *env, jclass cls) {
    (void)cls;
    static char buf[16384];  // (one caller: the panel's UI thread)
    panel_snapshot(buf, sizeof buf);
    return (*env)->NewStringUTF(env, buf);
}
