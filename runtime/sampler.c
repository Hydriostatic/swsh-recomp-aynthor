// LARECOMP_SAMPLE=<from frame>:<to frame> (Windows): a sampling profile of the game threads between those game frames
// (renderer_game_frame: presents the game has made). About 1,000 times a second each guest thread is suspended, its host
// stack unwound (the recompiled functions have unwind data like any other), and resumed; per thread, the functions on
// top (self) and anywhere on the stack (inclusive) are counted. Written to build/sample.txt (LARECOMP_SAMPLE_OUT) when
// the window ends: game functions as f_<guest addr>, the runtime's as lasrecomp+<offset> (tools/addr2sym.py / nm).
// Samples taken while the thread is in an svc count as "in svc" (blocked or in the kernel), not as its own work.
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "rt.h"

uint64_t renderer_game_frame(void);
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);
KThread *kernel_thread_list(void);

#define SMAX_THREADS 64
#define SDEPTH 96
#define SHASH (1 << 16)
typedef struct { uint64_t key, self, incl; } Ent;  // key: thread slot << 56 | host function start
static Ent *g_tab;
static struct { uint64_t id, samples, in_svc; } g_th[SMAX_THREADS];
static int g_nth;

static Ent *ent(uint64_t key) {
    for (uint64_t h = (key * 0x9E3779B97F4A7C15ull) >> 48;; h = (h + 1) & (SHASH - 1)) {
        if (g_tab[h].key == key) return &g_tab[h];
        if (!g_tab[h].key) { g_tab[h].key = key; return &g_tab[h]; }
    }
}

static int slot_of(uint64_t id) {
    for (int i = 0; i < g_nth; i++)
        if (g_th[i].id == id) return i;
    if (g_nth == SMAX_THREADS) return -1;
    g_th[g_nth].id = id;
    return g_nth++;
}

// Host function start of each frame of a suspended thread.
static int unwind(HANDLE h, uint64_t *fns) {
    CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
    if (!GetThreadContext(h, &ctx)) return 0;
    int n = 0;
    while (n < SDEPTH && ctx.Rip) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (!fe) {  // a leaf without unwind data: the return address is on top of the stack
            fns[n++] = ctx.Rip;
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)ctx.Rsp, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
            ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        fns[n++] = base + fe->BeginAddress;
        void *hd;
        DWORD64 est[2];
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &hd, est, NULL);
    }
    return n;
}

static int by_self(const void *a, const void *b) {  // (descending)
    uint64_t x = (*(Ent *const *)a)->self, y = (*(Ent *const *)b)->self;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int by_incl(const void *a, const void *b) {
    uint64_t x = (*(Ent *const *)a)->incl, y = (*(Ent *const *)b)->incl;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(uint64_t from, uint64_t to) {
    const char *path = getenv("LARECOMP_SAMPLE_OUT") ? getenv("LARECOMP_SAMPLE_OUT") : "build/sample.txt";
    FILE *f = fopen(path, "w");
    if (!f) { LOG("[sample] cannot write %s\n", path); return; }
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    fprintf(f, "# sampling profile, game frames %llu-%llu\n", (unsigned long long)from, (unsigned long long)to);
    Ent **v = malloc(SHASH * sizeof *v);
    for (int s = 0; s < g_nth; s++) {
        if (g_th[s].samples < 20) continue;
        fprintf(f, "\n## thread %llu: %llu samples, %.1f%% in svc\n", (unsigned long long)g_th[s].id,
                (unsigned long long)g_th[s].samples, 100.0 * g_th[s].in_svc / g_th[s].samples);
        for (int pass = 0; pass < 2; pass++) {
            int n = 0;
            for (int i = 0; i < SHASH; i++)
                if (g_tab[i].key && (int)(g_tab[i].key >> 56) == s && (pass ? g_tab[i].incl : g_tab[i].self)) v[n++] = &g_tab[i];
            qsort(v, n, sizeof *v, pass ? by_incl : by_self);
            fprintf(f, "%s:\n", pass ? "inclusive" : "self");
            for (int i = 0; i < n && i < 60; i++) {
                uint64_t pc = v[i]->key & ((1ull << 56) - 1), off = 0, c = pass ? v[i]->incl : v[i]->self;
                const char *name = rt_host_fn_name(pc, &off);
                char buf[96];
                if (name && !off) snprintf(buf, sizeof buf, "%s", name);
                else {
                    HMODULE m = NULL;
                    char mod[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)(uintptr_t)pc, &m)) {
                        GetModuleFileNameA(m, mod, sizeof mod);
                        char *b = strrchr(mod, '\\');
                        if (b) memmove(mod, b + 1, strlen(b));
                    }
                    snprintf(buf, sizeof buf, "%s+%llx", m == (HMODULE)(uintptr_t)exe ? "lasrecomp" : mod,
                             (unsigned long long)(pc - (uint64_t)(uintptr_t)m));
                }
                fprintf(f, "  %6.2f%% %7llu  %s\n", 100.0 * c / g_th[s].samples, (unsigned long long)c, buf);
            }
        }
    }
    free(v);
    fclose(f);
    LOG("[sample] profile written to %s\n", path);
}

// Host threads of the runtime sampled too (sampler_add_thread: the render thread), reported as thread 1000 + n.
static struct { HANDLE h; uint64_t id; } g_host[8];
static volatile int g_nhost;
void sampler_add_thread(void) {
    if (!getenv("LARECOMP_SAMPLE") || g_nhost == 8) return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (!h) return;
    int n = __atomic_fetch_add(&g_nhost, 1, __ATOMIC_SEQ_CST);
    g_host[n].id = 1000 + n;
    __atomic_store_n(&g_host[n].h, h, __ATOMIC_RELEASE);
}

static void sample_one(HANDLE h, uint64_t id, bool in_svc, uint64_t *fns) {
    if (SuspendThread(h) == (DWORD)-1) return;
    int n = unwind(h, fns);
    ResumeThread(h);
    int s = slot_of(id);
    if (s < 0 || !n) return;
    g_th[s].samples++;
    if (in_svc) { g_th[s].in_svc++; return; }
    ent((uint64_t)s << 56 | fns[0])->self++;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) dup = fns[j] == fns[i];
        if (!dup) ent((uint64_t)s << 56 | fns[i])->incl++;
    }
}

static void *sampler_main(void *arg) {
    const char *e = arg;
    uint64_t from = strtoull(e, NULL, 10), to = strchr(e, ':') ? strtoull(strchr(e, ':') + 1, NULL, 10) : from + 600;
    g_tab = calloc(SHASH, sizeof *g_tab);
    timeBeginPeriod(1);
    while (renderer_game_frame() < from) Sleep(5);
    LOG("[sample] sampling game threads from frame %llu\n", (unsigned long long)renderer_game_frame());
    static uint64_t fns[SDEPTH];
    while (renderer_game_frame() < to) {
        for (KThread *t = kernel_thread_list(); t; t = t->anext) {
            if (!t->started || t->finished || !t->host) continue;
            sample_one((HANDLE)t->host, t->id, t->cur_svc >= 0, fns);
        }
        for (int i = 0; i < g_nhost; i++)
            if (g_host[i].h) sample_one(g_host[i].h, g_host[i].id, false, fns);
        Sleep(1);
        static uint64_t rounds;
        if (++rounds % 1000 == 0) LOG("[sample] %llu rounds, frame %llu\n", (unsigned long long)rounds, (unsigned long long)renderer_game_frame());
    }
    report(from, renderer_game_frame());
    return NULL;
}

void sampler_start(void) {
    static bool started;
    const char *e = getenv("LARECOMP_SAMPLE");
    if (!e || started) return;
    started = true;
    pthread_t th;
    pthread_create(&th, NULL, sampler_main, (void *)e);
    pthread_detach(th);
}
#else
void sampler_start(void) {}
void sampler_add_thread(void) {}
#endif
