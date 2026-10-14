package com.gxr.mic;

import android.media.AudioRecordingConfiguration;
import android.media.IAudioService;
import android.media.IRecordingConfigDispatcher;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ServiceManager;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.InputStreamReader;
import java.util.List;

/**
 * Event-driven mic boost for Virtual Desktop.
 *
 * VD captures with AUDIO_SOURCE_VOICE_PERFORMANCE; the screen recorder and other apps use other
 * sources but share the same codec path (handset-mic -> TX_DEC0), and the vendor HAL ignores
 * software capture effects. So the codec gain is raised only while a VOICE_PERFORMANCE recording
 * is active (AudioService recording callbacks, no polling), relative to whatever value the HAL's
 * mixer path set, and put back afterwards.
 *
 * MODE=1: boost whenever VD records (concurrent recorders sharing the mic get it too).
 * MODE=2: boost only while VD is the only mic recorder.
 */
public final class Main {
    private static final String CTL = "TX_DEC0 Volume";
    private static final int CTL_MAX = 124;               // 0..124, 1 dB/step
    private static final int SRC_REMOTE_SUBMIX = 8;
    private static final int SRC_VOICE_PERFORMANCE = 10;
    private static final int SRC_ECHO_REFERENCE = 1997;
    private static final int SRC_FM_TUNER = 1998;
    private static final int SRC_HOTWORD = 1999;
    // The HAL applies the mixer path when the stream starts, which may land after the callback.
    private static final long[] RECHECK_MS = {0, 150, 500, 1500, 4000};

    private static int mode = 1;
    private static int gainDb = 14;
    private static String stateFile;
    private static Handler handler;
    private static volatile boolean want;
    private static int saved = -1;    // value the HAL had set before we boosted
    private static int written = -1;  // value we wrote (-1 = not boosted)

    private static final Runnable APPLY = Main::apply;

    public static void main(String[] args) throws Exception {
        loadConfig(args.length > 0 ? args[0] + "/config.conf" : null);
        stateFile = args.length > 0 ? args[0] + "/state" : null;
        loadState();
        log("start mode=" + mode + " gain=" + gainDb + "dB" + (written >= 0 ? " (resumed boost)" : ""));
        Looper.prepareMainLooper();
        handler = new Handler(Looper.getMainLooper());

        IBinder b;
        while ((b = ServiceManager.getService("audio")) == null) Thread.sleep(1000);
        IAudioService audio = IAudioService.Stub.asInterface(b);
        b.linkToDeath(() -> { log("audio service died"); restoreNow(); System.exit(1); }, 0);
        Runtime.getRuntime().addShutdownHook(new Thread(Main::restoreNow));

        audio.registerRecordingCallback(new IRecordingConfigDispatcher.Stub() {
            @Override
            public void dispatchRecordingConfigChange(List<AudioRecordingConfiguration> configs) {
                handler.post(() -> update(configs));
            }
        });
        update(audio.getActiveRecordingConfigurations());
        Looper.loop();
    }

    private static void update(List<AudioRecordingConfiguration> configs) {
        boolean vd = false, other = false;
        if (configs != null) {
            for (AudioRecordingConfiguration c : configs) {
                if (c.isClientSilenced()) continue;
                int s = c.getClientAudioSource();
                if (s == SRC_VOICE_PERFORMANCE) vd = true;
                else if (s != SRC_REMOTE_SUBMIX && s != SRC_ECHO_REFERENCE && s != SRC_FM_TUNER && s != SRC_HOTWORD) other = true;
            }
        }
        boolean w = vd && (mode == 1 || !other);
        if (w != want) log((w ? "boost on" : "boost off") + " (vd=" + vd + " other=" + other + ")");
        want = w;
        handler.removeCallbacks(APPLY);
        for (long d : RECHECK_MS) handler.postDelayed(APPLY, d);
    }

    private static synchronized void apply() {
        int cur = readCtl();
        if (cur < 0) return;
        if (want) {
            if (cur == written) return;               // still boosted
            saved = cur;                              // HAL (re)applied its path value
            written = Math.min(CTL_MAX, cur + gainDb);
            if (written != cur) writeCtl(written);
            saveState();
        } else if (written >= 0) {
            if (cur == written && saved >= 0) writeCtl(saved); // otherwise the HAL already reset it
            written = saved = -1;
            saveState();
        }
    }

    // Persisted so a daemon restart mid-session doesn't stack a second boost on top of ours.
    private static void loadState() {
        if (stateFile == null) return;
        try (BufferedReader r = new BufferedReader(new FileReader(stateFile))) {
            String[] p = r.readLine().trim().split(" ");
            int s = Integer.parseInt(p[0]), w = Integer.parseInt(p[1]);
            if (w >= 0 && readCtl() == w) { saved = s; written = w; }
        } catch (Exception ignored) { }
    }

    private static void saveState() {
        if (stateFile == null) return;
        try (FileWriter f = new FileWriter(stateFile)) {
            f.write(saved + " " + written + "\n");
        } catch (Exception e) {
            log("state: " + e);
        }
    }

    private static synchronized void restoreNow() {
        want = false;
        apply();
    }

    private static int readCtl() {
        try {
            Process p = new ProcessBuilder("tinymix", "-D", "0", "-v", CTL).redirectErrorStream(true).start();
            String line;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) { line = r.readLine(); }
            p.waitFor();
            return line == null ? -1 : Integer.parseInt(line.trim());
        } catch (Exception e) {
            log("read failed: " + e);
            return -1;
        }
    }

    private static void writeCtl(int v) {
        try {
            new ProcessBuilder("tinymix", "-D", "0", CTL, Integer.toString(v)).redirectErrorStream(true).start().waitFor();
        } catch (Exception e) {
            log("write failed: " + e);
        }
    }

    private static void loadConfig(String path) {
        if (path == null) return;
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            for (String l; (l = r.readLine()) != null; ) {
                l = l.trim();
                int eq = l.indexOf('=');
                if (l.startsWith("#") || eq < 0) continue;
                String k = l.substring(0, eq).trim(), v = l.substring(eq + 1).trim();
                try {
                    if (k.equals("MODE")) mode = Integer.parseInt(v) == 2 ? 2 : 1;
                    else if (k.equals("GAIN_DB")) gainDb = Math.max(0, Math.min(40, Integer.parseInt(v)));
                } catch (NumberFormatException ignored) { }
            }
        } catch (Exception e) {
            log("config: " + e + " (using defaults)");
        }
    }

    private static void log(String s) {
        System.out.println(System.currentTimeMillis() / 1000 + " " + s);
    }
}
