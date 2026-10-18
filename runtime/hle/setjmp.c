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
