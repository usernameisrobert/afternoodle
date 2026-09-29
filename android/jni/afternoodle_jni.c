/* ============================================================================
 *  afternoodle engine  --  Android JNI bridge
 * ============================================================================
 *  The smallest useful binding between the engine and a Java host: enough to
 *  ask the engine what it is, run its checks, and hand a rendered frame back
 *  as pixels.
 *
 *  There is no SDL_AndroidActivity here and no window. The bridge runs the
 *  engine on SDL's dummy video driver, which gives a real software renderer
 *  with no surface, so a frame can be produced and read back inside an
 *  ordinary Activity with no GL context and no Java glue on SDL's side. That
 *  keeps the engine testable on a device and keeps the APK tiny.
 *
 *  A real on-screen renderer needs SDL's own Android project (SDLActivity plus
 *  the SDL Java classes) built against this engine. That is a separate piece
 *  of work; nothing here blocks it.
 * ============================================================================
 */
#include <jni.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>
#include <SDL.h>

#include "afndle/core/afconfig.h"
#include "afndle/core/log.h"
#include "afndle/platform/platform.h"
#include "afndle/render/render.h"
#include "afndle/render/texture.h"

/* Engine symbols are compiled with -fvisibility=hidden so that a shared
 * engine only exports what it means to. JNIEXPORT carries default visibility,
 * which is all that is needed here. */
#define AFNDLE_JNI JNIEXPORT

static void jni_throw(JNIEnv *env, const char *msg) {
    jclass cls = (*env)->FindClass(env, "java/lang/RuntimeException");
    if (cls) (*env)->ThrowNew(env, cls, msg);
}

/* Drains the engine logger into logcat, so a failure in here is diagnosable
 * with `adb logcat -s afndle` rather than invisible. */
static void log_to_logcat(AfLogLevel level, const char *msg, void *user) {
    (void)user;
    int prio;
    switch (level) {
        case AF_LOG_TRACE:
        case AF_LOG_DEBUG: prio = ANDROID_LOG_DEBUG;   break;
        case AF_LOG_INFO:  prio = ANDROID_LOG_INFO;    break;
        case AF_LOG_WARN:  prio = ANDROID_LOG_WARN;    break;
        default:           prio = ANDROID_LOG_ERROR;   break;
    }
    __android_log_print(prio, "afndle", "%s", msg);
}

/* Both entry points below need a headless engine, and must not race if the UI
 * ever calls them from two threads. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_up = 0;

/* Brings up the platform on the dummy drivers. Idempotent. Returns 0 on
 * success. */
static int ensure_engine(void) {
    if (g_up) return 1;
    /* Set before SDL_Init: the video driver is chosen there, so afterwards is
     * too late. */
    if (!SDL_getenv("SDL_VIDEODRIVER")) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!SDL_getenv("SDL_AUDIODRIVER")) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);

    af_log_init(AF_LOG_DEBUG, 0 /* no ANSI colour in logcat */);
    af_log_add_sink(log_to_logcat, NULL);
    af_platform_init(AFNDLE_ENGINE_NAME);
    if (!af_platform_video_init()) {
        AF_ERROR("android: video init failed: %s", SDL_GetError());
        af_log_remove_sink(log_to_logcat, NULL);
        af_log_shutdown();
        return 0;
    }
    g_up = 1;
    return 1;
}

static void teardown_engine(void) {
    if (!g_up) return;
    af_platform_shutdown();
    af_log_remove_sink(log_to_logcat, NULL);
    af_log_shutdown();
    g_up = 0;
}


/* --------------------------------------------------------------------------
 *  version
 * -------------------------------------------------------------------------- */

AFNDLE_JNI jstring JNICALL
Java_com_afternoodle_engine_Afternoodle_nativeVersion(JNIEnv *env, jclass cls) {
    (void)cls;
    return (*env)->NewStringUTF(env, AFNDLE_VERSION_STRING);
}

/* --------------------------------------------------------------------------
 *  self test
 * -------------------------------------------------------------------------- */

/* Runs the engine's checks and returns a JSON report, so the Java side has
 * something structured to show instead of scraping stdout. */
AFNDLE_JNI jstring JNICALL
Java_com_afternoodle_engine_Afternoodle_nativeSelfTest(JNIEnv *env, jclass cls) {
    (void)cls;
    pthread_mutex_lock(&g_lock);

    if (!ensure_engine()) {
        pthread_mutex_unlock(&g_lock);
        jni_throw(env, "afternoodle: engine init failed");
        return NULL;
    }

    /* The same binary the makefiles build and CI runs, so the device is
     * testing the engine rather than a reimplementation of its checks. */
    extern int af_test_engine_main(void);
    int failures = af_test_engine_main();

    char summary[256];
    snprintf(summary, sizeof(summary),
             "{\"engine\":\"%s\",\"version\":\"%s\",\"failures\":%d,\"ok\":%s}",
             AFNDLE_ENGINE_NAME, AFNDLE_VERSION_STRING, failures,
             failures == 0 ? "true" : "false");

    teardown_engine();
    pthread_mutex_unlock(&g_lock);
    return (*env)->NewStringUTF(env, summary);
}

/* --------------------------------------------------------------------------
 *  render a frame
 * -------------------------------------------------------------------------- */

/* Draws one frame at w x h and returns it as tightly packed RGBA bytes, top
 * row first, which is what Bitmap.setPixels wants.
 *
 * The engine's own readback is ABGR8888 (byte order R,G,B,A in memory), which
 * is already what Bitmap.ARGB_8888 expects after a setPixels call, so the
 * words are copied across verbatim rather than swizzled. */
AFNDLE_JNI jbyteArray JNICALL
Java_com_afternoodle_engine_Afternoodle_nativeRenderFrame(JNIEnv *env, jclass cls,
                                                          jint w, jint h) {
    (void)cls;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        jni_throw(env, "afternoodle: bad frame size");
        return NULL;
    }

    pthread_mutex_lock(&g_lock);
    if (!ensure_engine()) {
        pthread_mutex_unlock(&g_lock);
        jni_throw(env, "afternoodle: engine init failed");
        return NULL;
    }

    AfWindowDesc d = af_window_desc_default();
    d.width = (int)w;
    d.height = (int)h;
    d.vsync = 0;
    d.hidden = 1;
    AfWindow *win = af_window_create(&d);
    if (!win) {
        teardown_engine();
        pthread_mutex_unlock(&g_lock);
        jni_throw(env, "afternoodle: could not create the render window");
        return NULL;
    }

    AfRenderer *r = af_window_renderer(win);
    /* The dummy driver picks its own window size, so the frame is captured at
     * the size that was asked for and the result is what the caller gets. */
    float sx = af_r2d_render_scale(r);
    if (sx <= 0.0f) sx = 1.0f;

    af_r2d_begin(r, af_color_hex(0x101018FF));
    af_r2d_fill(r, af_rect(0, 0, (float)w, (float)h * 0.35f),
                af_color_hex(0x1E2438FF));
    af_r2d_ui_fill(r, af_rect(12, 12, (float)w - 24, 40),
                   af_color_hex(0x2E3A5CFF));
    af_r2d_ui_text(r, af_font_default(), af_v2(24, 22), "afternoodle",
                   af_color_hex(0xF0F4FFFF), 26.0f);
    af_r2d_ui_text(r, af_font_default(), af_v2(24, 62),
                   "engine " AFNDLE_VERSION_STRING, af_color_hex(0x9FB0D0FF),
                   16.0f);

    /* A checker, a circle and a rotated quad, so the frame exercises the
     * batching paths rather than just clearing. */
    AfTexture *checker =
        af_texture_checker(16, af_color_hex(0x3D4E7AFF), af_color_hex(0x26304AFF));
    for (int i = 0; i < 6; i++) {
        float x = 24.0f + (float)(i % 3) * 68.0f;
        float y = 104.0f + (float)(i / 3) * 68.0f;
        af_r2d_ui_tex(r, checker, af_rect(x, y, 64, 64),
                      af_rect(0, 0, 1, 1), af_color_white());
    }
    af_r2d_ui_fill(r, af_rect(236, 104, 64, 64), af_color_hex(0xE0603AFF));
    af_r2d_ui_fill(r, af_rect(312, 168, 64, 64), af_color_hex(0x4CD07AFF));
    af_r2d_end(r);

    size_t bytes = (size_t)w * (size_t)h * 4;
    jbyteArray out = (*env)->NewByteArray(env, (jsize)bytes);
    unsigned char *px = NULL;
    if (out) px = (unsigned char *)malloc(bytes);

    int ok = px && af_r2d_read_pixels(r, px, (int)w, (int)h);
    if (ok) (*env)->SetByteArrayRegion(env, out, 0, (jsize)bytes,
                                       (const jbyte *)px);

    free(px);
    af_texture_destroy(checker);
    af_window_destroy(win);
    teardown_engine();
    pthread_mutex_unlock(&g_lock);

    if (!ok) {
        if (out) (*env)->DeleteLocalRef(env, out);
        jni_throw(env, "afternoodle: frame readback failed");
        return NULL;
    }
    return out;
}

/* There is deliberately no JNI_OnLoad here.
 *
 * SDL2 for Android defines one, and linking SDL2 statically into this library
 * makes it part of libafndle.so, so a second definition is a duplicate symbol
 * at link time. SDL's is the one worth having anyway: it caches the JavaVM
 * and calls SDL_SetMainReady(), which the engine relies on. The version this
 * bridge used to return was all it did, and SDL's does strictly more.
 */

