#include "afndle/core/time.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(AF_OS_WINDOWS)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#include "afndle/core/log.h"
#include "afndle/core/math.h"
#include "afndle/core/mem.h"

/* ------------------------------------------------------------------ clock */

static struct {
    double    origin;        /* wall clock at init */
    uint64_t  freq;
    double    paused_acc;    /* accumulated time while paused */
    int       paused;
    double    pause_started;
    char      label[32];
} g_clock;

static double now_seconds(void) {
#if defined(AF_OS_WINDOWS)
    LARGE_INTEGER f, c;
    if (!g_clock.freq) {
        QueryPerformanceFrequency(&f);
        g_clock.freq = (uint64_t)f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_clock.freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

#if !defined(AF_OS_WINDOWS)
static void ensure_init(void) __attribute__((constructor));
#endif
static int g_clock_ready;

static void clock_init_once(void) {
    if (g_clock_ready) return;
    g_clock_ready = 1;
    g_clock.origin = now_seconds();
#if !defined(AF_OS_WINDOWS)
    g_clock.freq = 1000000000ull;
#endif
    /* Init the wall-clock label used as a log prefix. */
    time_t t = time(NULL);
    struct tm tmv;
#if defined(AF_OS_WINDOWS)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(g_clock.label, sizeof(g_clock.label), "%H:%M:%S", &tmv);
}

#if !defined(AF_OS_WINDOWS)
/* Prime the clock before main() so the first af_time_now() is sane. */
static void ensure_init(void) { clock_init_once(); }
#endif
double af_time_now(void) {
    clock_init_once();
    return now_seconds() - g_clock.origin;
}

double af_time_game(void) {
    clock_init_once();
    double t = af_time_now() - g_clock.paused_acc;
    if (g_clock.paused) t -= (now_seconds() - g_clock.pause_started - g_clock.origin);
    return t;
}

void af_time_set_paused(int paused) {
    clock_init_once();
    if ((int)g_clock.paused == paused) return;
    if (paused) {
        g_clock.pause_started = now_seconds();
    } else {
        g_clock.paused_acc += now_seconds() - g_clock.pause_started;
    }
    g_clock.paused = paused;
}

int af_time_paused(void) { return g_clock.paused; }

uint64_t af_time_ms(void) { return (uint64_t)(af_time_now() * 1000.0); }

const char *af_time_label(void) {
    clock_init_once();
    /* Append milliseconds. */
    static char out[40];
    double frac = af_time_now() - floor(af_time_now());
    snprintf(out, sizeof(out), "%s.%03d", g_clock.label, (int)(frac * 1000.0));
    return out;
}

void af_sleep(double seconds) {
    if (seconds <= 0) return;
#if defined(AF_OS_WINDOWS)
    Sleep((DWORD)(seconds * 1000.0));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

/* ------------------------------------------------------------ frame timer */

void af_frametime_init(AfFrameTime *ft, int max_fps) {
    memset(ft, 0, sizeof(*ft));
    ft->time_scale = 1.0;
    ft->max_fps = max_fps;
    ft->fps = 60.0;
    clock_init_once();
    ft->frame_start = af_time_now();
}

void af_frametime_reset(AfFrameTime *ft) {
    double keep_scale = ft->time_scale;
    int keep_max = ft->max_fps;
    memset(ft, 0, sizeof(*ft));
    ft->time_scale = keep_scale;
    ft->max_fps = keep_max;
    ft->fps = 60.0;
    ft->frame_start = af_time_now();
}

void af_frametime_tick(AfFrameTime *ft, double dt) {
    /* Mark the start of the frame so sleep_for() can budget against it. */
    ft->frame_start = af_time_now();
    if (dt <= 0.0) dt = 1.0 / 60.0;

    /* Clamp pathological spikes (alt-tab, breakpoints, phone OS pause). */
    const double kMaxDt = 0.25;
    if (dt > kMaxDt) {
        ft->hit_spike = 1;
        dt = kMaxDt;
    }

    ft->raw_dt = dt;
    ft->dt = af_lerpf(ft->dt, dt, AF_FRAME_SMOOTHING);
    ft->elapsed += dt * ft->time_scale;
    ft->real_elapsed += dt;
    ft->frame++;

    if (ft->dt > 1e-6) {
        double inst = 1.0 / ft->dt;
        ft->fps = af_lerpf(ft->fps, inst, 0.10);
        if (inst < ft->fps_min || ft->frame == 1) ft->fps_min = inst;
        if (inst > ft->fps_max || ft->frame == 1) ft->fps_max = inst;
    }
}

double af_frametime_sleep_for(AfFrameTime *ft) {
    if (ft->max_fps <= 0) return 0.0;
    double target = 1.0 / (double)ft->max_fps;
    double budget = target - (af_time_now() - ft->frame_start);
    return budget > 0.0 ? budget : 0.0;
}

/* --------------------------------------------------------- fixed stepper */

void af_fixedstep_init(AfFixedStep *fs, double step_hz, int max_steps) {
    fs->accumulator = 0.0;
    fs->step = step_hz > 0.0 ? 1.0 / step_hz : 1.0 / 60.0;
    fs->max_steps = max_steps > 0 ? max_steps : 5;
}

int af_fixedstep_begin(AfFixedStep *fs, double frame_dt) {
    if (frame_dt < 0.0) frame_dt = 0.0;
    fs->accumulator += frame_dt;
    int steps = (int)(fs->accumulator / fs->step);
    if (steps > fs->max_steps) {
        /* Drop the backlog: better a small time skip than a death spiral. */
        steps = fs->max_steps;
        fs->accumulator = 0.0;
    } else {
        fs->accumulator -= (double)steps * fs->step;
    }
    if (fs->accumulator > fs->step) fs->accumulator = fs->step;
    return steps;
}

/* -------------------------------------------------------------- stopwatch */

void af_stopwatch_start(AfStopwatch *s) { s->start_ns = (uint64_t)(af_time_now() * 1e9); }

double af_stopwatch_elapsed(AfStopwatch *s) {
    return af_time_now() - (double)s->start_ns * 1e-9;
}

/* ----------------------------------------------------------------- timer */

struct AfTimer {
    double duration;
    double elapsed;
    int    active;
    int    finished;
};

AfTimer *af_timer_create(double duration) {
    AfTimer *t = (AfTimer *)af_calloc(1, sizeof(AfTimer));
    t->duration = duration;
    return t;
}

void af_timer_destroy(AfTimer *t) { af_free(t); }
void af_timer_reset(AfTimer *t, double duration) {
    t->duration = duration;
    t->elapsed = 0.0;
    t->active = 0;
    t->finished = 0;
}
void af_timer_start(AfTimer *t) {
    t->elapsed = 0.0;
    t->active = 1;
    t->finished = 0;
}
void af_timer_stop(AfTimer *t) { t->active = 0; }

void af_timer_update(AfTimer *t, double dt) {
    if (!t->active) return;
    t->elapsed += dt;
    if (t->duration <= 0.0 || t->elapsed >= t->duration) {
        t->elapsed = t->duration;
        t->active = 0;
        t->finished = 1;
    }
}

int af_timer_active(const AfTimer *t)    { return t->active; }
int af_timer_finished(const AfTimer *t)  { return t->finished; }
float af_timer_progress(const AfTimer *t) {
    return t->duration > 0.0 ? (float)(t->elapsed / t->duration) : 1.0f;
}
double af_timer_remaining(const AfTimer *t) {
    double r = t->duration - t->elapsed;
    return r > 0.0 ? r : 0.0;
}
