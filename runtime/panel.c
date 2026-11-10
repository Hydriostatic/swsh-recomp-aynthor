// Second-screen information (panel.h).
#include "panel.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gamesetup.h"
#include "settings.h"

#define MAX_ITEMS 128
typedef struct { char section[32], key[48], value[96]; } Item;
static Item g_items[MAX_ITEMS];
static int g_nitems;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_state;

void panel_state(int s) { atomic_store(&g_state, s); }

void panel_set(const char *section, const char *key, const char *fmt, ...) {
    char v[96] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(v, sizeof v, fmt, ap);
        va_end(ap);
        for (char *c = v; *c; c++) if (*c == '\t' || *c == '\n') *c = ' ';
    }
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g_nitems && (strcmp(g_items[i].section, section) || strcmp(g_items[i].key, key))) i++;
    if (!fmt) {
        if (i < g_nitems) { memmove(&g_items[i], &g_items[i + 1], (size_t)(g_nitems - i - 1) * sizeof *g_items); g_nitems--; }
    } else if (i < g_nitems || g_nitems < MAX_ITEMS) {
        if (i == g_nitems) {
            g_nitems++;
            snprintf(g_items[i].section, sizeof g_items[i].section, "%s", section);
            snprintf(g_items[i].key, sizeof g_items[i].key, "%s", key);
        }
        memcpy(g_items[i].value, v, sizeof v);
    }
    pthread_mutex_unlock(&g_lock);
}

void panel_clear(const char *section) {
    pthread_mutex_lock(&g_lock);
    int n = 0;
    for (int i = 0; i < g_nitems; i++)
        if (strcmp(g_items[i].section, section)) g_items[n++] = g_items[i];
    g_nitems = n;
    pthread_mutex_unlock(&g_lock);
}

uint64_t renderer_present_count(void);           // nvn/vulkan/device.c
int shader_prep_progress(float *frac);           // nvn/vulkan/prep.c
const char *vk_gpu_name(void);                   // nvn/vulkan/device.c

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

// Frames shown per second over the last second or so (from the presents counter: no hook in the frame loop)
static double fps_now(double *frame_ms) {
    static uint64_t t0, f0;
    static double fps, ms;
    uint64_t t = now_ns(), f = renderer_present_count();
    if (!t0) t0 = t, f0 = f;
    if (t - t0 >= 500000000ull) {
        double dt = (double)(t - t0) / 1e9;
        fps = (double)(f - f0) / dt;
        ms = f > f0 ? dt * 1000.0 / (double)(f - f0) : 0;
        t0 = t, f0 = f;
    }
    *frame_ms = ms;
    return fps;
}

static long rss_mb(void) {  // resident memory of the process
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, res = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0; fclose(f); }
    return res * 4096 / (1 << 20);
}

#define ADD(...) do { if (n < size) n += (size_t)snprintf(out + n, size - n, __VA_ARGS__); } while (0)

size_t panel_snapshot(char *out, size_t size) {
    size_t n = 0;
    if (!size) return 0;
    out[0] = 0;
    int st = atomic_load(&g_state);
    const char *title = game_title("extracted/exefs");
    ADD("status\tstate\t%s\n", st == PANEL_PLAYING ? "Playing" : st == PANEL_BOOTING ? "Starting" : "Start screen");
    ADD("status\tgame\t%s\n", title ? title : "No game set up");
    if (title) ADD("status\tversion\t%s\n", game_version("extracted/exefs"));
    if (st == PANEL_PLAYING) {
        double ms, fps = fps_now(&ms);
        ADD("performance\tfps\t%.0f\n", fps);
        ADD("performance\ttarget\t%d\n", g_settings.fps);
        ADD("performance\tframe time\t%.1f ms\n", ms);
    }
    float frac = 0;
    int prep = shader_prep_progress(&frac);
    if (prep == 1) ADD("performance\tshaders\tpreparing %.0f%%\n", frac * 100);
    else if (prep == 2) ADD("performance\tshaders\tready\n");
    ADD("system\tmemory\t%ld MB\n", rss_mb());
    const char *gpu = vk_gpu_name();
    if (gpu && *gpu) ADD("system\tgpu\t%s\n", gpu);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nitems; i++) ADD("%s\t%s\t%s\n", g_items[i].section, g_items[i].key, g_items[i].value);
    pthread_mutex_unlock(&g_lock);
    if (n >= size) n = size - 1;
    return n;
}
