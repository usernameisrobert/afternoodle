/* afternoodle engine -- platform detection, export macros, version.
 *
 * This header is included by everything. Keep it dependency-free.
 */
#ifndef AFNDLE_CORE_AFCONFIG_H
#define AFNDLE_CORE_AFCONFIG_H

/* ---------------------------------------------------------------- version */
#define AFNDLE_VERSION_MAJOR 0
#define AFNDLE_VERSION_MINOR 1
#define AFNDLE_VERSION_PATCH 0
#define AFNDLE_VERSION_STRING "0.1.0"
#define AFNDLE_ENGINE_NAME    "afternoodle"

/* ------------------------------------------------------------------ OSes */
#if defined(_WIN32) || defined(_WIN64)
#  define AF_OS_WINDOWS 1
#elif defined(__ANDROID__)
#  define AF_OS_ANDROID 1
#elif defined(__APPLE__) && defined(__MACH__) && defined(TARGET_OS_IPHONE)
#  define AF_OS_IOS 1
#elif defined(__linux__)
#  define AF_OS_LINUX 1
#elif defined(__APPLE__)
#  define AF_OS_MACOS 1
#else
#  define AF_OS_UNKNOWN 1
#endif

/* MinGW links msvcrt, whose printf predates C99 and has no %zu. The engine
 * only ever formats sizes, never parses them, so printing them as unsigned
 * long long works on every target and costs nothing. */
#define AF_SIZE_FMT "llu"
#define AF_SIZE_ARG(x) ((unsigned long long)(x))

/* Windows DLL export/import decoration. */
#if defined(AF_OS_WINDOWS)
#  if defined(AFNDLE_BUILDING)
#    define AF_API __declspec(dllexport)
#  elif defined(AFNDLE_STATIC)
#    define AF_API
#  else
#    define AF_API __declspec(dllimport)
#  endif
#  define AF_LOCAL
#else
#  if defined(AFNDLE_BUILDING)
#    define AF_API __attribute__((visibility("default")))
#  else
#    define AF_API
#  endif
#  define AF_LOCAL __attribute__((visibility("hidden")))
#endif

/* --------------------------------------------------------------- helpers */
#define AF_ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define AF_UNUSED(x)     ((void)(x))
#define AF_STRINGIFY_(x) #x
#define AF_STRINGIFY(x)  AF_STRINGIFY_(x)

/* Inline keyword that works on MSVC C mode too. */
#if defined(_MSC_VER)
#  define AF_INLINE __inline
#else
#  define AF_INLINE static inline
#endif

/* noreturn, portable. */
#if defined(__GNUC__)
#  define AF_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#  define AF_NORETURN __declspec(noreturn)
#else
#  define AF_NORETURN
#endif

/* Function name for logging / asserts. */
#if defined(__GNUC__)
#  define AF_FUNC_NAME __func__
#elif defined(_MSC_VER)
#  define AF_FUNC_NAME __FUNCTION__
#else
#  define AF_FUNC_NAME "fn"
#endif

/* Marks a printf-style function for -Wformat checking. */
#if defined(__GNUC__)
#  define AF_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#  define AF_PRINTF(fmt, first)
#endif

/* Branch prediction hints. */
#if defined(__GNUC__)
#  define AF_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define AF_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define AF_LIKELY(x)   (x)
#  define AF_UNLIKELY(x) (x)
#endif

#endif /* AFNDLE_CORE_AFCONFIG_H */
