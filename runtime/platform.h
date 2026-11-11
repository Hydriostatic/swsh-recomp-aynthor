// Host OS layer: everything the runtime needs that differs between macOS and Windows.
// Implementations: runtime/platform/macos.c, runtime/platform/windows.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#elif defined(__ANDROID__)
#include "platform/android_compat.h"
#elif defined(__linux__)
#include "platform/linux_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim [lo, hi) for the guest at exactly that address, readable/writable (pages may be backed lazily).
bool plat_reserve_guest(uint64_t lo, uint64_t hi);
// Pages of the guest window written since the last call for that range (4 KB granularity), reset by the call: up to
// max page addresses into out; returns their number, max when there may be more (treat the range as written), or -1
// when the platform does not track writes (LARECOMP_WRITE_WATCH=0, or no support): then nothing can be assumed.
long plat_written_pages(uint64_t addr, uint64_t size, uint64_t *out, long max);
// Make sure [addr, addr+size) inside the guest window is backed (before the OS itself writes there, e.g.
// file reads); a no-op where the whole window is backed from the start.
void plat_commit(uint64_t addr, uint64_t size);
// In-place reboot: every page of [lo, hi) back to zero (and unbacked), the window still claimed.
bool plat_reset_guest(uint64_t lo, uint64_t hi);
// A large host-only region, readable/writable, its pages backed (zeroed) on first touch: NULL if it can't be had.
// One per process (the NVN command records' shadow, runtime/nvn/cmdbuf.c).
void *plat_reserve_lazy(uint64_t size);
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t addr, uint64_t size, int prot);  // 0 on success
#define PLAT_PAGE 0x4000ull  // largest page size we deal with (macOS 16K; multiple of Windows' 4K)

// ---- time
uint64_t plat_now_ns(void);                // monotonic
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t
void plat_thread_realtime(uint64_t period_ns);  // periodic timing thread (vsync): don't coalesce its timers
void plat_thread_background(void);              // background work (shader builds): below the game and render threads
void plat_thread_idle(void);                    // work that may wait (shader preparation): runs only when the CPU is idle
#ifdef _WIN32
// A guest thread's precise wait (kernel.c kwait): an auto-reset event its wakers set, and a high-resolution timer for the
// timeout. MinGW's pthread_cond_timedwait / nanosleep round short waits up to the 15.6 ms system tick on Windows even
// with timeBeginPeriod(1): 1 / 3 / 8 ms took 16.0 / 16.3 / 16.4 ms here (tests/runtime/timed_wait_test.c). Technique from
// SkyRecomp (MIT, docs/engineering/recomp-project-comparison.md).
void *plat_waiter_create(void);
void plat_waiter_destroy(void *w);
void plat_waiter_reset(void *w);
void plat_waiter_signal(void *w);
bool plat_waiter_wait(void *w, int64_t timeout_ns);  // -1: no timeout; true when signalled, false on timeout
#endif

// ---- process
void plat_init(void);                                // first thing in main()
bool plat_exe_path(char *buf, size_t size);
void plat_exec(char *const argv[]);  // replace the process (NULL = same command line); returns only on failure
#if defined(_WIN32) || defined(__linux__)
void plat_terminate(void);  // end the process now, skipping DLL teardown (exit status 0)
#endif

// Child processes (seed generator, game-file extraction). stdout + stderr go to log_path (NULL = /dev/null).
typedef intptr_t PlatProc;  // 0 = failed to start
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log);
int plat_proc_wait(PlatProc p, bool block);  // exit code, or -1 while still running (block = false)
void plat_proc_kill(PlatProc p);             // end it and what it started (then it's gone: no wait)
bool plat_remove_tree(const char *dir);      // rm -rf
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#define PLAT_VENV_PYTHON "venv/Scripts/python.exe"  // relative to the venv's parent
#else
#define PLAT_EXE_SUFFIX ""
#define PLAT_VENV_PYTHON "venv/bin/python"
#endif

#ifdef __ANDROID__
// ---- Android (runtime/platform/android.c)
// The game code library at src (shared storage) copied where it can be loaded from; out: the path to load.
bool plat_android_stage_lib(const char *src, char *out, size_t n);
const char *plat_android_data_dir(void);       // the data root: Android/data/<package>/files on shared storage
bool plat_android_in_background(void);         // the activity is paused: its window has no surface to draw into
int plat_android_surface_generation(void);     // counts the returns to the foreground (each brings a new surface)
// Opens the system folder picker (after asking for all-files access); the folder's path comes back, on another
// thread, to launcher_android_folder_picked (runtime/ui/launcher_lg.mm).
void plat_android_pick_folder(void);
#endif

// ---- diagnostics
// Name of the host function containing pc (guest functions are f_<addr>), NULL if unknown.
const char *plat_symbol(uint64_t pc, uint64_t *offset);
void plat_print_backtrace(void);
void plat_fatal_notice(void);  // a fatal error is about to end the process: tell the user (Windows release: message box)
void plat_crash_handler_install(void);  // per thread where the OS needs it (alternate signal stack)
// Called by the crash handler (runtime/dispatch.c): true = fault handled (watchpoint), resume.
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
