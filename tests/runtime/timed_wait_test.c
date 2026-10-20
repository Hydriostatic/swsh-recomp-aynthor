// timed_wait_test.c: how long a requested 1 / 3 / 8 ms timed wait really takes with the primitives runtime/kernel.c
// uses on Windows (MinGW winpthreads pthread_cond_timedwait and nanosleep), with timeBeginPeriod(1) as the runtime sets.
// SkyRecomp measured ~15.6 ms for 3 ms waits; this checks our toolchain and machine.
//   clang -O1 tests/runtime/timed_wait_test.c -lwinmm -o build/timed_wait_test.exe && build/timed_wait_test.exe
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <windows.h>

static double now_ms(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(void) {
    timeBeginPeriod(1);
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    int req[] = {1, 3, 8};
    for (int r = 0; r < 3; r++) {
        double sum_cv = 0, max_cv = 0, sum_ns = 0, max_ns = 0;
        const int n = 40;
        for (int i = 0; i < n; i++) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += req[r] * 1000000L;
            if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
            pthread_mutex_lock(&m);
            double t0 = now_ms();
            pthread_cond_timedwait(&cv, &m, &ts);
            double dt = now_ms() - t0;
            pthread_mutex_unlock(&m);
            sum_cv += dt; if (dt > max_cv) max_cv = dt;
            struct timespec s = {0, req[r] * 1000000L};
            t0 = now_ms();
            nanosleep(&s, NULL);
            dt = now_ms() - t0;
            sum_ns += dt; if (dt > max_ns) max_ns = dt;
        }
        printf("requested %d ms: pthread_cond_timedwait mean %.2f max %.2f | nanosleep mean %.2f max %.2f\n", req[r],
               sum_cv / n, max_cv, sum_ns / n, max_ns);
    }
    return 0;
}
