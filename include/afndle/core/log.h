/* Logging + assertions. Thread safe, colour aware, level filterable. */
#ifndef AFNDLE_CORE_LOG_H
#define AFNDLE_CORE_LOG_H

#include "afndle/core/afconfig.h"

typedef enum {
    AF_LOG_TRACE = 0,
    AF_LOG_DEBUG,
    AF_LOG_INFO,
    AF_LOG_WARN,
    AF_LOG_ERROR,
    AF_LOG_FATAL,
    AF_LOG_NONE
} AfLogLevel;

/* Installs stderr/stdout sinks. Optional; af_log works without it. */
AF_API void        af_log_init(AfLogLevel min_level, int use_color);
AF_API void        af_log_shutdown(void);
AF_API AfLogLevel  af_log_level(void);
AF_API void        af_log_set_level(AfLogLevel level);

/* Extra sinks: the editor uses this to show console output in a panel. */
typedef void (*AfLogSinkFn)(AfLogLevel level, const char *msg, void *user);
AF_API void af_log_add_sink(AfLogSinkFn fn, void *user);
AF_API void af_log_remove_sink(AfLogSinkFn fn, void *user);

AF_API void af_log_write(AfLogLevel level, const char *file, int line,
                         const char *fmt, ...) AF_PRINTF(4, 5);

#define AF_TRACE(...) af_log_write(AF_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define AF_DEBUG(...) af_log_write(AF_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define AF_INFO(...)  af_log_write(AF_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define AF_WARN(...)  af_log_write(AF_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define AF_ERROR(...) af_log_write(AF_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define AF_FATAL(...) af_log_write(AF_LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

/* ------------------------------------------------------------ assert/panic */
AF_API AF_NORETURN void af_panic(const char *file, int line, const char *fmt, ...)
    AF_PRINTF(3, 4);

#if defined(NDEBUG)
#  define AF_ASSERT(cond) ((void)0)
#else
#  define AF_ASSERT(cond)                                                        \
      do {                                                                        \
          if (AF_UNLIKELY(!(cond))) af_panic(__FILE__, __LINE__,                  \
                                              "assertion failed: %s", #cond);     \
      } while (0)
#endif

#define AF_ASSERT_MSG(cond, ...)                                                 \
    do {                                                                         \
        if (AF_UNLIKELY(!(cond))) af_panic(__FILE__, __LINE__, __VA_ARGS__);      \
    } while (0)

#define AF_UNREACHABLE() af_panic(__FILE__, __LINE__, "unreachable code reached")

#endif /* AFNDLE_CORE_LOG_H */
