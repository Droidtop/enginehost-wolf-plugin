package dev.enginehost.plugin.wolf;

import android.opengl.GLSurfaceView;
import android.os.ParcelFileDescriptor;
import android.view.ViewGroup;
import android.widget.FrameLayout;
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
                @Override public void onDrawFrame(GL10 unused) { nativeGlDraw(running, System.nanoTime()); }
            });
            view.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
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
        return engine == 0 || view != null ? -1 : nativeStep(engine, pixels);
    }

    // No input yet.
    @Override public void onPointerMove(int x, int y) {}
    @Override public void onPointerUp(int x, int y) {}

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
    static native int nativeStep(long engine, int[] pixels);
    static native void nativeStop(long engine);
    static native void nativeGlCreated(long engine);
    static native void nativeGlSize(long engine, int width, int height);
    static native void nativeGlDraw(long engine, long nowNs);
    static native void nativePause(long engine, boolean paused);
}
