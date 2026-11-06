package dev.enginehost.plugin.wolf;

import android.annotation.SuppressLint;
import android.opengl.GLSurfaceView;
import android.view.MotionEvent;
import android.os.ParcelFileDescriptor;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EngineHost;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import dev.enginehost.api.EngineProcess;
import dev.enginehost.api.EngineStepDriven;
import java.io.IOException;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * The WOLF RPG Editor core (Droidtop/tracker#858). The game folder is read through Enginehost's
 * file layer (Enginehost's plugin-native/), so the core opens exactly what the host hands it.
 *
 * <p>Two ways to run (comment 6691411927):
 * <ul>
 *   <li><b>With a display</b> (Enginehost's runtime process, allocate-then-clamp): a
 *       {@link GLSurfaceView} in the display; its GL thread runs the game's frames and draws them
 *       with GLES 2. Sound goes out through AAudio. When the Fibrewall GPU proxy lands, this path
 *       moves into the isolated process (#815).
 *   <li><b>Isolated, step-driven</b> (the fallback): the host calls {@link #step} once a frame and
 *       draws the pixels the core's software renderer fills in; sound goes into the host's audio
 *       ring.
 * </ul>
 *
 * <p>A game the core refuses (Pro, a protect key) or cannot read ends the launch through
 * {@link EngineHost#fail} with the core's own sentence.
 */
public final class WolfPlugin implements EnginePlugin, EngineStepDriven {
    static {
        try {
            System.loadLibrary("wolfrpg");
        } catch (UnsatisfiedLinkError e) {
            // Under isolation the host loads the library from a descriptor and binds these
            // natives itself (enginehost_register_natives); System.loadLibrary cannot find it
            // there and does not need to.
            if (!EngineProcess.isIsolated()) throw e;
        }
    }

    private long engine;
    private GLSurfaceView view;

    // The keys held, as the core's WOLF_KEY_* bits (vm.h), from Enginehost's controller actions.
    // A tap on the screen is one confirm press, held for one game frame.
    private static final int KEY_CONFIRM = 0x01, KEY_CANCEL = 0x02, KEY_SUB = 0x04,
            KEY_UP = 0x08, KEY_DOWN = 0x10, KEY_LEFT = 0x20, KEY_RIGHT = 0x40;
    private volatile int keys;
    private volatile boolean tapped;

    /** The keys for the next game frame: what is held, plus a pending tap (taken once). */
    private int takeKeys() {
        int k = keys;
        if (tapped) {
            tapped = false;
            k |= KEY_CONFIRM;
        }
        return k;
    }

    private void setKey(int bit, boolean down) {
        synchronized (this) {
            keys = down ? keys | bit : keys & ~bit;
        }
    }

    @Override
    public void onCreate(EnginePluginSession session) throws Exception {
        EngineHost host = session.host();
        ViewGroup display = session.display();
        if (display != null) {
            engine = nativeStart(session.gamePath(), -1, 0, true);
            if (engine == 0) {
                fail(host);
                return;
            }
            view = new GLSurfaceView(display.getContext());
            view.setEGLContextClientVersion(2);
            view.setPreserveEGLContextOnPause(true);
            final long running = engine;
            view.setRenderer(new GLSurfaceView.Renderer() {
                @Override public void onSurfaceCreated(GL10 unused, EGLConfig config) { nativeGlCreated(running); }
                @Override public void onSurfaceChanged(GL10 unused, int width, int height) {
                    nativeGlSize(running, width, height);
                }
                @Override public void onDrawFrame(GL10 unused) {
                    nativeGlDraw(running, System.nanoTime(), takeKeys());
                }
            });
            view.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
            view.setOnTouchListener(new android.view.View.OnTouchListener() {
                @SuppressLint("ClickableViewAccessibility")
                @Override public boolean onTouch(android.view.View v, MotionEvent event) {
                    if (event.getActionMasked() == MotionEvent.ACTION_UP) tapped = true;
                    return true;
                }
            });
            display.addView(view, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            return;
        }
        // Isolated: sound goes into the host's audio ring (Enginehost plugin-native/
        // enginehost_audio_ring); the native side maps it, so the descriptor can be closed after.
        ParcelFileDescriptor ring = host.isolatedAudioBuffer();
        try {
            engine = nativeStart(session.gamePath(), ring == null ? -1 : ring.getFd(),
                    host.isolatedAudioSampleRate(), false);
        } finally {
            if (ring != null) ring.close();
        }
        if (engine == 0) fail(host);
    }

    private static void fail(EngineHost host) throws IOException {
        String reason = nativeError();
        try {
            host.fail(reason);
        } catch (IncompatibleClassChangeError olderHost) {
            // An Enginehost without fail(): throwing from onCreate shows the same reason.
            throw new IOException(reason);
        }
    }

    @Override public int pixelWidth() { return engine == 0 ? 0 : nativeWidth(engine); }
    @Override public int pixelHeight() { return engine == 0 ? 0 : nativeHeight(engine); }

    @Override
    public int step(int[] pixels) {
        return engine == 0 || view != null ? -1 : nativeStep(engine, pixels, takeKeys());
    }

    // Isolated: a pointer lifted is a tap.
    @Override public void onPointerMove(int x, int y) {}
    @Override public void onPointerUp(int x, int y) { tapped = true; }

    /** Enginehost's standard actions (after the person's own remapping) to Wolf's keys. */
    @Override
    public boolean onControllerEvent(EngineControllerEvent event) {
        switch (event.action()) {
            case "confirm": setKey(KEY_CONFIRM, event.pressed()); return true;
            case "cancel":
            case "menu": setKey(KEY_CANCEL, event.pressed()); return true;
            case "skip": setKey(KEY_SUB, event.pressed()); return true;
            case "up": setKey(KEY_UP, event.pressed()); return true;
            case "down": setKey(KEY_DOWN, event.pressed()); return true;
            case "left": setKey(KEY_LEFT, event.pressed()); return true;
            case "right": setKey(KEY_RIGHT, event.pressed()); return true;
            case "left_x":
                setKey(KEY_LEFT, event.value() < -0.5f);
                setKey(KEY_RIGHT, event.value() > 0.5f);
                return true;
            case "left_y":
                setKey(KEY_UP, event.value() < -0.5f);
                setKey(KEY_DOWN, event.value() > 0.5f);
                return true;
            default:
                return false;
        }
    }

    @Override
    public void onResume() {
        if (view != null) view.onResume();
        if (engine != 0) nativePause(engine, false);
    }

    @Override
    public void onPause() {
        if (view != null) view.onPause(); // returns once the GL thread has paused
        if (engine != 0) nativePause(engine, true);
    }

    @Override
    public void onDestroy() {
        if (view != null) {
            view.onPause(); // the GL thread stops drawing before the engine goes
            ViewGroup parent = (ViewGroup) view.getParent();
            if (parent != null) parent.removeView(view);
            view = null;
        }
        if (engine != 0) nativeStop(engine);
        engine = 0;
    }

    static native long nativeStart(String gamePath, int audioRingFd, int audioSampleRate, boolean display);
    static native String nativeError();
    static native int nativeWidth(long engine);
    static native int nativeHeight(long engine);
    static native int nativeStep(long engine, int[] pixels, int keys);
    static native void nativeStop(long engine);
    static native void nativeGlCreated(long engine);
    static native void nativeGlSize(long engine, int width, int height);
    static native void nativeGlDraw(long engine, long nowNs, int keys);
    static native void nativePause(long engine, boolean paused);
}
