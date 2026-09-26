package com.afternoodle.engine;

/**
 * The engine's Java side.
 *
 * <p>Everything here is a thin declaration of a native entry point in
 * {@code android/jni/afternoodle_jni.c}. There is no SDL Activity and no GL
 * context: the engine runs on SDL's dummy video driver, so a frame can be
 * produced and read back inside an ordinary Activity. That is enough to
 * exercise the renderer on a real device and to show output, and it keeps the
 * APK free of SDL's own Java glue.
 *
 * <p>Pixel order is the engine's own. {@code af_r2d_read_pixels} fills ABGR8888
 * words, which in memory are R,G,B,A bytes -- exactly the order Android's
 * {@code ARGB_8888} bitmaps store -- so a frame crosses the JNI boundary and
 * reaches a {@code Bitmap} with no conversion and no per-pixel work in Java.
 */
public final class Afternoodle {

    static {
        // Packed into lib/arm64-v8a/libafndle.so by the APK build.
        System.loadLibrary("afternoodle");
    }

    private Afternoodle() {
    }

    /** The engine's version string, e.g. {@code "0.1.0"}. */
    public static native String nativeVersion();

    /**
     * Runs the engine's own check suite, the same binary the makefiles build
     * and CI runs.
     *
     * @return a JSON summary, e.g. {@code {"failures":0,"ok":true,...}}
     */
    public static native String nativeSelfTest();

    /**
     * Draws one frame with the real renderer and reads it back.
     *
     * @param w frame width in pixels
     * @param h frame height in pixels
     * @return {@code w * h * 4} bytes, tightly packed R,G,B,A, top row first
     */
    public static native byte[] nativeRenderFrame(int w, int h);

    /** The engine version, for display. */
    public static String version() {
        return nativeVersion();
    }
}
