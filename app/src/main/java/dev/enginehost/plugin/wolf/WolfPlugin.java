package dev.enginehost.plugin.wolf;

import dev.enginehost.api.EngineHost;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import dev.enginehost.api.EngineProcess;
import dev.enginehost.api.EngineStepDriven;
import android.os.ParcelFileDescriptor;
import java.io.IOException;

/**
 * The WOLF RPG Editor core in Enginehost's sandbox (Enginehost docs/engine-sandbox.md,
 * Droidtop/tracker#858).
 *
 * <p>Step-driven in the isolated process: the host calls {@link #step} once a frame and draws
 * the pixels it fills in. The game folder is read through Enginehost's file layer
 * (Enginehost's plugin-native/), so the core opens exactly what the host hands it and nothing else.
 *
 * <p>First milestone: {@link #onCreate} loads the game's project data (Game.dat,
 * CommonEvent.dat, the three databases, unpacked or from Data.wolf) and logs it under the tag
 * "wolf"; each step is a black frame of the game's screen size. A game the core refuses (Pro, a
 * protect key) or cannot read ends the launch through {@link EngineHost#fail} with the core's
 * own sentence.
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

    @Override
    public void onCreate(EnginePluginSession session) throws Exception {
        if (session.display() != null) {
            throw new IOException("The Wolf core runs only in Enginehost's sandbox");
        }
        EngineHost host = session.host();
        // Sound goes into the host's audio ring (Enginehost plugin-native/enginehost_audio_ring);
        // the native side maps it, so the descriptor can be closed straight after.
        ParcelFileDescriptor ring = host.isolatedAudioBuffer();
        try {
            engine = nativeStart(session.gamePath(), ring == null ? -1 : ring.getFd(),
                    host.isolatedAudioSampleRate());
        } finally {
            if (ring != null) ring.close();
        }
        if (engine != 0) return;
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
        return engine == 0 ? -1 : nativeStep(engine, pixels);
    }

    // No input yet: the interpreter does not run in this milestone.
    @Override public void onPointerMove(int x, int y) {}
    @Override public void onPointerUp(int x, int y) {}

    @Override
    public void onDestroy() {
        if (engine != 0) nativeStop(engine);
        engine = 0;
    }

    static native long nativeStart(String gamePath, int audioRingFd, int audioSampleRate);
    static native String nativeError();
    static native int nativeWidth(long engine);
    static native int nativeHeight(long engine);
    static native int nativeStep(long engine, int[] pixels);
    static native void nativeStop(long engine);
}
