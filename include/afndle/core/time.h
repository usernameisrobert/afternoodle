/* Time: monotonic clock, frame timing, fixed-step accumulator, timers. */
#ifndef AFNDLE_CORE_TIME_H
#define AFNDLE_CORE_TIME_H

#include <stdint.h>
#include "afndle/core/afconfig.h"

/** Seconds since an arbitrary fixed point, monotonic. Never goes backwards. */
AF_API double af_time_now(void);
/** Same, but paused-aware: does not advance while af_time_set_paused(1). */
AF_API double af_time_game(void);
AF_API void   af_time_set_paused(int paused);
AF_API int    af_time_paused(void);
/** Milliseconds since engine start (wall clock, for window titles). */
AF_API uint64_t af_time_ms(void);
/** "HH:MM:SS.mmm" of wall-clock time, for log prefixes. */
AF_API const char* af_time_label(void);
/** Sleep for `seconds`; yields the thread. */
AF_API void   af_sleep(double seconds);

/* ------------------------------------------------------------ frame timer */
typedef struct {
    double   dt;          /* smoothed frame delta, seconds */
    double   raw_dt;      /* unsmoothed delta, seconds */
    double   elapsed;     /* total simulated time */
    double   real_elapsed;/* total wall time since reset */
    double   time_scale;  /* 1 = normal, 0 = frozen, >1 = fast forward */
    double   fps;
    double   fps_min;
    double   fps_max;
    float    alpha;       /* interpolation factor between fixed steps */
    uint64_t frame;
    int      max_fps;     /* 0 = uncapped */
    int      hit_spike;   /* set when a frame took longer than 4x the average */
    double   frame_start; /* monotonic stamp when this frame began */
} AfFrameTime;

#define AF_FRAME_SMOOTHING 0.10 /* seconds of exponential smoothing */

AF_API void   af_frametime_init(AfFrameTime* ft, int max_fps);
/** Call once per frame with dt already measured (or 0 to auto-measure). */
AF_API void   af_frametime_tick(AfFrameTime* ft, double dt);
AF_API void   af_frametime_reset(AfFrameTime* ft);
/** Seconds to sleep before the next frame so we honour max_fps. */
AF_API double af_frametime_sleep_for(AfFrameTime* ft);

/* ------------------------------------------------------- fixed-step driver */
/* Splits variable frame time into whole fixed steps plus a remainder.
 *
 *   double acc = ft->dt;
 *   while (acc >= step) { step_world(step); acc -= step; }
 *   ft->alpha = acc / step;   // interpolate rendering
 */
typedef struct {
    double accumulator;
    double step;      /* fixed step, seconds */
    int    max_steps; /* spiral-of-death guard */
} AfFixedStep;

AF_API void   af_fixedstep_init(AfFixedStep* fs, double step_hz, int max_steps);
/** Returns number of fixed steps to run. Caller loops. */
AF_API int    af_fixedstep_begin(AfFixedStep* fs, double frame_dt);
AF_INLINE double af_fixedstep_alpha(const AfFixedStep* fs) {
    return fs->step > 0.0 ? fs->accumulator / fs->step : 0.0;
}

/* ------------------------------------------------------------- stopwatch */
typedef struct { uint64_t start_ns; } AfStopwatch;
AF_API void     af_stopwatch_start(AfStopwatch* s);
AF_API double   af_stopwatch_elapsed(AfStopwatch* s); /* seconds */

/* ---------------------------------------------------------------- timers */
/* Countdown timers that do not need a world; used for fades, cooldowns. */
typedef struct AfTimer AfTimer;
AF_API AfTimer* af_timer_create(double duration);
AF_API void     af_timer_destroy(AfTimer* t);
AF_API void     af_timer_reset(AfTimer* t, double duration);
AF_API void     af_timer_start(AfTimer* t);
AF_API void     af_timer_stop(AfTimer* t);
AF_API void     af_timer_update(AfTimer* t, double dt);
AF_API int      af_timer_active(const AfTimer* t);
AF_API int      af_timer_finished(const AfTimer* t);
AF_API float    af_timer_progress(const AfTimer* t); /* 0..1 */
AF_API double   af_timer_remaining(const AfTimer* t);

#endif /* AFNDLE_CORE_TIME_H */
