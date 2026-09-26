#include "afndle/core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afndle/core/mem.h"
#include "afndle/core/str.h"
#include "afndle/core/time.h"

#if defined(AF_OS_WINDOWS)
#  include <windows.h>
#endif

#define AF_MAX_LOG_SINKS 8

/* Implemented in src/platform; kept as a weak-ish extern so core does not need
 * to include the platform header. */
int af_platform_stdout_is_tty(void);

typedef struct {
    AfLogSinkFn fn;
    void*       user;
} LogSink;

static struct {
    AfLogLevel  min_level;
    int         color;
    int         initialised;
    LogSink     sinks[AF_MAX_LOG_SINKS];
    int         sink_count;
    uint64_t    warn_count;
    uint64_t    error_count;
} g_log;

static const char *const k_level_names[] = {"TRACE", "DEBUG", "INFO",
                                            "WARN",  "ERROR", "FATAL"};
static const char *const k_level_colors[] = {"\033[90m", "\033[36m", "\033[32m",
                                             "\033[33m", "\033[31m", "\033[35m"};
#define AF_ANSI_RESET "\033[0m"

void af_log_init(AfLogLevel min_level, int use_color) {
    g_log.min_level = min_level;
    g_log.color = use_color;
    g_log.initialised = 1;
    /* Default to colour when stdout is a TTY. */
    g_log.color = af_platform_stdout_is_tty();
}

void af_log_shutdown(void) {
    g_log.sink_count = 0;
    g_log.initialised = 0;
}

AfLogLevel af_log_level(void) { return g_log.min_level; }

void af_log_set_level(AfLogLevel level) { g_log.min_level = level; }

void af_log_add_sink(AfLogSinkFn fn, void *user) {
    if (!fn || g_log.sink_count >= AF_MAX_LOG_SINKS) return;
    g_log.sinks[g_log.sink_count].fn = fn;
    g_log.sinks[g_log.sink_count].user = user;
    g_log.sink_count++;
}

void af_log_remove_sink(AfLogSinkFn fn, void *user) {
    for (int i = 0; i < g_log.sink_count; i++) {
        if (g_log.sinks[i].fn == fn && g_log.sinks[i].user == user) {
            g_log.sinks[i] = g_log.sinks[g_log.sink_count - 1];
            g_log.sink_count--;
            return;
        }
    }
}

const char* af_log_level_name(AfLogLevel l) {
    if ((int)l < 0 || (int)l > AF_LOG_FATAL) return "?";
    return k_level_names[l];
}

void af_log_write(AfLogLevel level, const char *file, int line, const char *fmt,
                  ...) {
    if (!g_log.initialised) af_log_init(AF_LOG_INFO, 0);
    if (level < g_log.min_level) return;

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    if (level == AF_LOG_WARN) g_log.warn_count++;
    if (level >= AF_LOG_ERROR) g_log.error_count++;

    /* Console sink. */
    const char* base = file ? strrchr(file, '/') : NULL;
    base = base ? base + 1 : file;
    int idx = (int)level;
    if (g_log.color) {
        fprintf(stderr,
                "\033[90m[%s" AF_ANSI_RESET " %s%-5s" AF_ANSI_RESET " \033[90m%s:%d"
                AF_ANSI_RESET " %s\n",
                af_time_label(), k_level_colors[idx], k_level_names[idx],
                base ? base : "?", line, body);
    } else {
        fprintf(stderr, "[%s] %-5s %s:%d %s\n", af_time_label(), k_level_names[idx],
                base ? base : "?", line, body);
    }
    fflush(stderr);

    /* Extra sinks (editor console panel). */
    for (int i = 0; i < g_log.sink_count; i++)
        g_log.sinks[i].fn(level, body, g_log.sinks[i].user);
}

void af_panic(const char *file, int line, const char *fmt, ...) {
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    const char* base = file ? strrchr(file, '/') : NULL;
    base = base ? base + 1 : file;
    fprintf(stderr, "\n\033[1;31m=== PANIC ===\033[0m %s:%d\n%s\n", base ? base : "?",
            line, body);
    fflush(stderr);
    af_log_shutdown();
    abort();
}

uint64_t af_log_warning_count(void)  { return g_log.warn_count; }
uint64_t af_log_error_count(void)    { return g_log.error_count; }
