// The game's setjmp / longjmp (nn's libc, imported by main as setjmp, _setjmp, longjmp, _longjmp). Sword 1.3.2's
// script engine catches errors with them at boot (a protected call: setjmp, run, longjmp back on error), and the
// recompiled code can't take a longjmp on its own: the guest one ends in a jump to the instruction after the setjmp
// call - the middle of a guest function, where there is no host function to dispatch to ("no function at
// 7001664c94", docs/HISTORICO.md item 63).
//
// Here setjmp keeps two things, by jmp_buf and thread:
//  - the guest's callee-saved state (x19-x30, sp, v8-v15), what the guest longjmp would have restored;
//  - the host context of the recompiled function that called setjmp, as it is right after that call returns: the
//    current context unwound (RtlVirtualUnwind) until the frame of that function. That frame is still live when
//    longjmp comes (C's rule for setjmp), so restoring the context resumes it after the call, like on the console.
// longjmp puts the guest state back, x0 = the value (1 for 0), and restores the host context.
#ifdef _WIN32
#include <windows.h>

#include "../gamelib.h"
#include "../hle.h"

typedef struct {
    uint64_t buf;  // the guest jmp_buf (0: free slot)
    DWORD tid;
    uint64_t x[12], sp;  // x19..x30
    __uint128_t v[8];    // v8..v15
    CONTEXT host;
} Jmp;
#define NJMP 512
static Jmp g_jmp[NJMP];
static unsigned g_next;
static SRWLOCK g_lock = SRWLOCK_INIT;

// The host function of the guest function holding guest address a (0: none).
static uint64_t host_fn_at(uint64_t a) {
    const GameLibTables *t = gamelib_tables();
    for (uint32_t m = 0; t && m < t->nmods; m++) {
        const GameLibModule *md = &t->mods[m];
        uint64_t n = *md->count;
        if (!n || a < md->addrs[0] || a > md->addrs[n - 1] + 0x100000) continue;
        uint64_t lo = 0, hi = n;  // last start <= a
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (md->addrs[mid] <= a) lo = mid + 1;
            else hi = mid;
        }
        if (lo) return (uint64_t)(uintptr_t)md->fns[lo - 1];
    }
    return 0;
}

static Jmp *slot(uint64_t buf, DWORD tid, bool create) {
    for (int i = 0; i < NJMP; i++)
        if (g_jmp[i].buf == buf && g_jmp[i].tid == tid) return &g_jmp[i];
    if (!create) return NULL;
    Jmp *j = &g_jmp[g_next++ % NJMP];  // (the oldest: a protected call that returned long ago)
    j->buf = buf, j->tid = tid;
    return j;
}

static void do_setjmp(Cpu *c) {
    uint64_t caller = host_fn_at(c->x[30]);
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    bool found = false;
    for (int depth = 0; caller && depth < 32; depth++) {
        DWORD64 base;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (!rf) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; continue; }  // (a leaf: return address on top)
        if (base + rf->BeginAddress == caller) { found = true; break; }
        PVOID handler;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler, &frame, NULL);
    }
    if (!found) {
        static int logged;
        if (logged++ < 4) LOG("[setjmp] no host frame for the caller at %llx: a longjmp to it would fail\n",
                              (unsigned long long)c->x[30]);
        c->x[0] = 0;
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Jmp *j = slot(c->x[0], GetCurrentThreadId(), true);
    memcpy(j->x, &c->x[19], sizeof j->x);
    j->sp = c->sp;
    memcpy(j->v, &c->v[8], sizeof j->v);
    j->host = ctx;
    ReleaseSRWLockExclusive(&g_lock);
    c->x[0] = 0;
}

static void do_longjmp(Cpu *c) {
    AcquireSRWLockShared(&g_lock);
    Jmp *j = slot(c->x[0], GetCurrentThreadId(), false);
    ReleaseSRWLockShared(&g_lock);
    if (!j) FATAL("longjmp to %llx: no setjmp for it on this thread\n", (unsigned long long)c->x[0]);
    static int logged;
    if (logged++ < 4) LOG("[setjmp] longjmp to %llx (value %llu), back to %llx\n", (unsigned long long)c->x[0],
                          (unsigned long long)c->x[1], (unsigned long long)j->x[11]);
    memcpy(&c->x[19], j->x, sizeof j->x);
    c->sp = j->sp;
    memcpy(&c->v[8], j->v, sizeof j->v);
    c->x[0] = c->x[1] ? c->x[1] : 1;
    CONTEXT host = j->host;  // (a copy: the slot may be reused by the setjmp the resumed code makes next)
    RtlRestoreContext(&host, NULL);
}

HLE_FUNC("setjmp", hle_setjmp) { do_setjmp(c); }
HLE_FUNC("_setjmp", hle__setjmp) { do_setjmp(c); }
HLE_FUNC("longjmp", hle_longjmp) { do_longjmp(c); }
HLE_FUNC("_longjmp", hle__longjmp) { do_longjmp(c); }
#endif

#if !defined(_WIN32) && defined(__aarch64__)
// Linux / Android on AArch64: the same with LLVM's libunwind (linked into the app: the NDK's and clang's runtime). At
// setjmp the current host context is unwound (unw_step, the runtime's own unwind tables - libmain.so: dispatch, the
// HLE call) up to the frame of the recompiled function that called setjmp, and that cursor - the function's state as
// right after the call returns - is kept; longjmp puts the guest state back and resumes it (unw_resume). The game
// code's own unwind tables aren't needed: every frame between setjmp and its caller is the runtime's.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../gamelib.h"
#include "../hle.h"

typedef uint64_t unw_word_t;
typedef struct { uint64_t data[160]; } unw_context_t;  // (libunwind's are 66 and 78 words on AArch64: room to spare)
typedef struct { uint64_t data[192]; } unw_cursor_t;
enum { UNW_REG_IP = -1, UNW_REG_SP = -2 };
int unw_getcontext(unw_context_t *);
int unw_init_local(unw_cursor_t *, unw_context_t *);
int unw_step(unw_cursor_t *);
int unw_get_reg(unw_cursor_t *, int, unw_word_t *);
int unw_resume(unw_cursor_t *);

typedef struct {
    uint64_t buf;  // the guest jmp_buf (0: free slot)
    pid_t tid;
    uint64_t x[12], sp;  // x19..x30
    __uint128_t v[8];    // v8..v15
    unw_cursor_t host;
} Jmp;
#define NJMP 512
static Jmp g_jmp[NJMP];
static unsigned g_next;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// The game code's host functions sorted by address: which one a host address is in (its start; 0: none)
static uintptr_t *g_starts;
static size_t g_nstarts;
static int cmp_ptr(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}
static uintptr_t host_fn_containing(uintptr_t ip) {
    if (!g_starts) {
        const GameLibTables *t = gamelib_tables();
        size_t n = 0;
        for (uint32_t m = 0; t && m < t->nmods; m++) n += *t->mods[m].count;
        uintptr_t *s = malloc(n * sizeof *s);
        if (!s) return 0;
        size_t k = 0;
        for (uint32_t m = 0; t && m < t->nmods; m++)
            for (uint64_t i = 0; i < *t->mods[m].count; i++) s[k++] = (uintptr_t)t->mods[m].fns[i];
        qsort(s, k, sizeof *s, cmp_ptr);
        g_nstarts = k;
        g_starts = s;
    }
    size_t lo = 0, hi = g_nstarts;  // last start <= ip
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_starts[mid] <= ip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? g_starts[lo - 1] : 0;
}

// The host function of the guest function holding guest address a (0: none)
static uintptr_t host_fn_at(uint64_t a) {
    const GameLibTables *t = gamelib_tables();
    for (uint32_t m = 0; t && m < t->nmods; m++) {
        const GameLibModule *md = &t->mods[m];
        uint64_t n = *md->count;
        if (!n || a < md->addrs[0] || a > md->addrs[n - 1] + 0x100000) continue;
        uint64_t lo = 0, hi = n;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (md->addrs[mid] <= a) lo = mid + 1;
            else hi = mid;
        }
        if (lo) return (uintptr_t)md->fns[lo - 1];
    }
    return 0;
}

static Jmp *slot(uint64_t buf, pid_t tid, bool create) {
    for (int i = 0; i < NJMP; i++)
        if (g_jmp[i].buf == buf && g_jmp[i].tid == tid) return &g_jmp[i];
    if (!create) return NULL;
    Jmp *j = &g_jmp[g_next++ % NJMP];  // (the oldest: a protected call that returned long ago)
    j->buf = buf, j->tid = tid;
    return j;
}

static __attribute__((noinline)) void do_setjmp(Cpu *c) {
    uintptr_t caller = host_fn_at(c->x[30]);
    static unw_context_t uc_;  // (big: not on the guest's host stack each time; filled and used under the lock)
    static unw_cursor_t cur_;
    pthread_mutex_lock(&g_lock);
    unw_context_t *uc = &uc_;
    unw_cursor_t *cur = &cur_;
    bool found = false;
    if (caller && unw_getcontext(uc) == 0 && unw_init_local(cur, uc) == 0) {
        for (int depth = 0; depth < 32; depth++) {
            if (unw_step(cur) <= 0) break;
            unw_word_t ip = 0;
            unw_get_reg(cur, UNW_REG_IP, &ip);
            if (host_fn_containing((uintptr_t)ip) == caller) { found = true; break; }
        }
    }
    if (!found) {
        pthread_mutex_unlock(&g_lock);
        static int logged;
        if (logged++ < 4) LOG("[setjmp] no host frame for the caller at %llx: a longjmp to it would fail\n",
                              (unsigned long long)c->x[30]);
        c->x[0] = 0;
        return;
    }
    Jmp *j = slot(c->x[0], gettid(), true);
    memcpy(j->x, &c->x[19], sizeof j->x);
    j->sp = c->sp;
    memcpy(j->v, &c->v[8], sizeof j->v);
    j->host = *cur;
    pthread_mutex_unlock(&g_lock);
    c->x[0] = 0;
}

static void do_longjmp(Cpu *c) {
    pthread_mutex_lock(&g_lock);
    Jmp *j = slot(c->x[0], gettid(), false);
    static __thread unw_cursor_t host;  // (a copy: the slot may be reused by the setjmp the resumed code makes next)
    if (j) host = j->host;
    pthread_mutex_unlock(&g_lock);
    if (!j) FATAL("longjmp to %llx: no setjmp for it on this thread\n", (unsigned long long)c->x[0]);
    static int logged;
    if (logged++ < 4) LOG("[setjmp] longjmp to %llx (value %llu), back to %llx\n", (unsigned long long)c->x[0],
                          (unsigned long long)c->x[1], (unsigned long long)j->x[11]);
    memcpy(&c->x[19], j->x, sizeof j->x);
    c->sp = j->sp;
    memcpy(&c->v[8], j->v, sizeof j->v);
    c->x[0] = c->x[1] ? c->x[1] : 1;
    unw_resume(&host);
    FATAL("longjmp to %llx: the host context couldn't be restored\n", (unsigned long long)c->x[0]);
}

HLE_FUNC("setjmp", hle_setjmp) { do_setjmp(c); }
HLE_FUNC("_setjmp", hle__setjmp) { do_setjmp(c); }
HLE_FUNC("longjmp", hle_longjmp) { do_longjmp(c); }
HLE_FUNC("_longjmp", hle__longjmp) { do_longjmp(c); }
#endif
