package com.afternoodle.app;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import java.nio.ByteBuffer;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.afternoodle.engine.Afternoodle;

/**
 * Shows the engine running on the device: a frame drawn by the real renderer
 * and the result of the engine's own check suite.
 *
 * <p>The native work happens off the main thread because the engine logs and
 * renders, and a frame is not instant. Both calls are made against a
 * {@code libafndle.so} that is statically linked to SDL, so nothing outside
 * the APK is needed at run time.
 */
public final class MainActivity extends Activity {

    private static final String TAG = "afternoodle";

    private ImageView frame;
    private TextView status;
    private TextView report;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setTitle("afternoodle");
        Afternoodle.nativeSetDataDir(getFilesDir().getAbsolutePath());

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#0B0D14"));
        int pad = dp(12);
        root.setPadding(pad, pad, pad, pad);

        status = new TextView(this);
        status.setTextColor(Color.parseColor("#9FB0D0"));
        status.setTextSize(13);
        root.addView(status, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        frame = new ImageView(this);
        frame.setScaleType(ImageView.ScaleType.FIT_CENTER);
        frame.setBackgroundColor(Color.parseColor("#101018"));
        LinearLayout.LayoutParams flp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0);
        flp.weight = 1f;
        flp.topMargin = dp(8);
        root.addView(frame, flp);

        report = new TextView(this);
        report.setTextColor(Color.parseColor("#D8E0F0"));
        report.setTextSize(12);
        report.setTypeface(Typeface.MONOSPACE);
        report.setTextIsSelectable(true);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(report, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 0.6f));

        setContentView(root);

        status.setText("starting engine " + Afternoodle.version() + " ...");
        // Nothing here may touch the view hierarchy, so it runs on a worker.
        new Thread(this::runEngine, "afternoodle-demo").start();
    }

    /** Brings the engine up off the main thread, then reports back. */
    private void runEngine() {
        String selfTest;
        Bitmap bitmap = null;
        String error = null;
        try {
            selfTest = Afternoodle.nativeSelfTest();
            // A third of the screen wide is enough to show the frame and keeps
            // the readback cheap on a phone.
            int w = 480;
            int h = 320;
            byte[] pixels = Afternoodle.nativeRenderFrame(w, h);
            // The engine hands back bytes in R,G,B,A order, which is what
            // ARGB_8888 stores in memory, so this is a straight copy.
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(pixels));
        } catch (Throwable t) {
            Log.e(TAG, "engine failed", t);
            error = t.toString();
            selfTest = null;
        }

        final String finalSelfTest = selfTest;
        final String finalError = error;
        final Bitmap finalBitmap = bitmap;
        runOnUiThread(() -> show(finalSelfTest, finalError, finalBitmap));
    }

    private void show(String selfTest, String error, Bitmap bitmap) {
        if (error != null) {
            status.setTextColor(Color.parseColor("#FF6B6B"));
            status.setText("engine failed");
            report.setText(error);
            return;
        }
        frame.setImageBitmap(bitmap);
        String summary = selfTest.replace(",\"", "\n  \"");
        report.setText(summary);
        boolean ok = selfTest.contains("\"ok\":true");
        status.setTextColor(Color.parseColor(ok ? "#4CD07A" : "#FFB020"));
        status.setText(ok ? "engine " + Afternoodle.version() + " - all checks passed"
                          : "engine " + Afternoodle.version() + " - CHECKS FAILED");
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
