package com.gxr.pt;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.Properties;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * gxr-passthrough-anywhere daemon (run as root via app_process).
 *
 * XrSystemUI rejects the headset touchpad double-tap while an app is in
 * FULL_SPACE_UNMANAGED mode (OpenXR / immersive). We tail SysUI's own log for that
 * rejection and then drive the global CPM passthrough node directly
 * (android.graphics.CPM.getPassthroughNode()), putting passthrough above the app
 * (z=9999). The app keeps running.
 *
 * usage: Main daemon <moduleDir> | on | off
 */
public final class Main {
    static final String TAG = "GxrPtAnywhere";
    static final String REJECT = "rejected in full space mode";
    static final String SYS_PUSH = "Setting XR Runtime Server PT layer to";
    static final Pattern P_PUSH = Pattern.compile("alpha=([-0-9.]+), z=(-?\\d+)");

    // CPM reflection
    static Class<?> cNode, cUpd;
    static Constructor<?> updCtor;
    static Method mGetPt, mAlpha, mZ, mVis, mApply;

    // config
    static float onAlpha = 1.0f;
    static int onZ = 9999;
    static long debounceMs = 600;
    static boolean enabled = true;
    static boolean muteToast = true;

    // state
    static volatile boolean forced = false;
    static volatile float sysAlpha = 0f;   // last state SysUI asked for
    static volatile int sysZ = -2;
    static long lastToggle = 0;

    public static void main(String[] a) throws Throwable {
        initCpm();
        String mode = a.length > 0 ? a[0] : "daemon";
        if ("on".equals(mode)) { applyOn(); return; }
        if ("off".equals(mode)) { apply(0f, -2, false); return; }
        String dir = a.length > 1 ? a[1] : "/data/adb/modules/gxr-passthrough-anywhere";
        loadConfig(new File(dir, "config.conf"));
        startCtlThread(new File(dir, "ctl"));
        // start from a clean toast state (in case a previous instance died while muted)
        exec("cmd", "appops", "set", "com.android.systemui", "TOAST_WINDOW", "allow");
        Runtime.getRuntime().addShutdownHook(new Thread(() ->
                exec("cmd", "appops", "set", "com.android.systemui", "TOAST_WINDOW", "allow")));
        startFullSpaceTracker();
        log("daemon started (alpha=" + onAlpha + " z=" + onZ + " enabled=" + enabled + " muteToast=" + muteToast + ")");
        logcatLoop();
    }

    static void initCpm() throws Throwable {
        Class<?> cpm = Class.forName("android.graphics.CPM");
        cNode = Class.forName("android.graphics.CPM$Node");
        cUpd = Class.forName("android.graphics.CPM$NodeUpdates");
        updCtor = cUpd.getConstructor();
        mGetPt = cpm.getMethod("getPassthroughNode");
        mAlpha = cUpd.getMethod("setAlpha", cNode, float.class);
        mZ = cUpd.getMethod("setZ", cNode, int.class);
        mVis = cUpd.getMethod("setVisible", cNode, boolean.class);
        mApply = cUpd.getMethod("apply");
    }

    static synchronized void apply(float alpha, int z, boolean visible) {
        try {
            Object n = mGetPt.invoke(null);
            Object u = updCtor.newInstance();
            mAlpha.invoke(u, n, alpha);
            mZ.invoke(u, n, z);
            mVis.invoke(u, n, visible);
            mApply.invoke(u);
        } catch (Throwable t) {
            log("CPM apply failed: " + t);
        }
    }

    static void applyOn() { apply(onAlpha, onZ, true); }

    static synchronized void setForced(boolean on, String why) {
        if (on) {
            applyOn();
        } else {
            // hand control back: restore what SysUI last requested
            apply(sysAlpha, sysZ, sysAlpha > 0f);
        }
        forced = on;
        log("passthrough " + (on ? "ON" : "OFF") + " (" + why + ")");
    }

    static synchronized void toggle(String why) {
        long now = System.currentTimeMillis();
        if (now - lastToggle < debounceMs) return;
        lastToggle = now;
        setForced(!forced, why);
    }

    // --- SysUI log follower -------------------------------------------------
    static void logcatLoop() {
        while (true) {
            Process p = null;
            try {
                p = new ProcessBuilder("logcat", "-v", "brief", "-T", "1",
                        "-s", "OnDeviceTouchpadInputEventReceiver:D", "SceneTransactionProcessorImpl:D")
                        .redirectErrorStream(true).start();
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = r.readLine()) != null) handleLine(line);
            } catch (Throwable t) {
                log("logcat loop error: " + t);
            } finally {
                if (p != null) p.destroy();
            }
            sleep(1000);
        }
    }

    // Each rejected double-tap enqueues one ~4.1 s SysUI toast (NMS shows them back-to-back).
    // A hidden toast becomes visible if TOAST_WINDOW is re-allowed before it expires, so keep
    // toasts muted until the queue has drained.
    static final long TOAST_SLOT_MS = 4600;
    static volatile long toastQueueEnd = 0;

    static void handleLine(String line) {
        if (line.contains(REJECT)) {
            long now = System.currentTimeMillis();
            toastQueueEnd = Math.max(toastQueueEnd, now) + TOAST_SLOT_MS;
            if (enabled) toggle("double-tap in full-space app");
            return;
        }
        int i = line.indexOf(SYS_PUSH);
        if (i >= 0) {
            Matcher m = P_PUSH.matcher(line);
            if (m.find(i)) {
                try {
                    sysAlpha = Float.parseFloat(m.group(1));
                    sysZ = Integer.parseInt(m.group(2));
                } catch (NumberFormatException ignored) { }
            }
            // SysUI changed passthrough itself (app exit, lockscreen, home toggle...):
            // it now owns the node again.
            if (forced) {
                synchronized (Main.class) {
                    forced = false;
                    if (sysAlpha <= 0f) apply(sysAlpha, sysZ, false);
                }
                log("SysUI took over passthrough (alpha=" + sysAlpha + " z=" + sysZ + ")");
            }
        }
    }

    // --- full-space tracking (TaskStackListener) ------------------------------
    static final int XR_FULL_SPACE_UNMANAGED = 2; // WindowConfiguration.XR_COMPUTED_DESKTOP_MODE_FULL_SPACE_UNMANAGED
    static volatile boolean fullSpace = false;
    static volatile boolean toastsMuted = false;
    static Object atm;
    static Method mFocused;
    static final Object evalLock = new Object();
    static volatile boolean evalPending = false;

    static final class Tsl extends android.app.TaskStackListener {
        @Override public void onTaskStackChanged() { scheduleEval(); }
    }

    static void startFullSpaceTracker() {
        try {
            Class<?> cAtm = Class.forName("android.app.ActivityTaskManager");
            atm = cAtm.getMethod("getService").invoke(null);
            Class<?> iAtm = Class.forName("android.app.IActivityTaskManager");
            mFocused = iAtm.getMethod("getFocusedRootTaskInfo");
            iAtm.getMethod("registerTaskStackListener", Class.forName("android.app.ITaskStackListener"))
                    .invoke(atm, new Tsl());
            log("TaskStackListener registered");
        } catch (Throwable t) {
            log("TaskStackListener failed, falling back to polling: " + t);
        }
        Thread t = new Thread(() -> {
            evaluate();
            while (true) {
                synchronized (evalLock) {
                    try { evalLock.wait(atm != null ? 5000 : 1500); } catch (InterruptedException ignored) { }
                    evalPending = false;
                }
                sleep(150); // coalesce bursts of task-stack callbacks
                evaluate();
            }
        }, "fullspace");
        t.setDaemon(true);
        t.start();
    }

    static void scheduleEval() {
        synchronized (evalLock) {
            if (!evalPending) { evalPending = true; evalLock.notifyAll(); }
        }
    }

    static int focusedXrMode() {
        try {
            Object info = mFocused.invoke(atm);
            if (info == null) return -1;
            Object cfg = info.getClass().getField("configuration").get(info);
            Object wc = cfg.getClass().getField("windowConfiguration").get(cfg);
            return (Integer) wc.getClass().getMethod("getComputedXrDesktopMode").invoke(wc);
        } catch (Throwable t) {
            return -2;
        }
    }

    static void evaluate() {
        boolean fs;
        if (atm != null && mFocused != null) {
            int m = focusedXrMode();
            if (m == -2) return; // query failed, keep previous state
            fs = (m == XR_FULL_SPACE_UNMANAGED);
        } else {
            String top = topActivityLine();
            if (top == null) return;
            fs = top.contains("full-space-unmanaged");
        }
        if (fs != fullSpace) {
            fullSpace = fs;
            log("full-space " + (fs ? "entered" : "left"));
        }
        long wait = toastQueueEnd - System.currentTimeMillis();
        if (fs && muteToast) setToastsMuted(true);
        else if (wait <= 0) setToastsMuted(false);
        else { final long w = wait; new Thread(() -> { sleep(w + 50); scheduleEval(); }).start(); }
        if (!fs && forced) {
            synchronized (Main.class) {
                if (forced) setForced(false, "full-space app left foreground");
            }
        }
    }

    static String topActivityLine() {
        Process p = null;
        try {
            p = new ProcessBuilder("dumpsys", "activity", "activities").redirectErrorStream(true).start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                if (line.contains("topResumedActivity=")) return line;
            }
        } catch (Throwable ignored) {
        } finally {
            if (p != null) p.destroy();
        }
        return null;
    }

    // XrSystemUI shows "Can't turn Virtual Environment on or off right now" on every rejected
    // double-tap. Its toasts are TYPE_TOAST windows owned by com.android.systemui, gated by
    // appop TOAST_WINDOW -> mute SysUI toasts only while a full-space app is focused.
    static synchronized void setToastsMuted(boolean mute) {
        if (mute == toastsMuted) return;
        if (exec("cmd", "appops", "set", "com.android.systemui", "TOAST_WINDOW", mute ? "ignore" : "allow")) {
            toastsMuted = mute;
            log("SysUI toasts " + (mute ? "muted" : "restored"));
        }
    }

    static boolean exec(String... cmd) {
        try {
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            p.getInputStream().close();
            return p.waitFor() == 0;
        } catch (Throwable t) {
            log("exec failed: " + t);
            return false;
        }
    }

    // --- control FIFO (action.sh / manual) ---------------------------------
    static void startCtlThread(File fifo) {
        Thread t = new Thread(() -> {
            while (true) {
                try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(fifo)))) {
                    String cmd;
                    while ((cmd = r.readLine()) != null) {
                        cmd = cmd.trim();
                        if (cmd.equals("toggle")) { lastToggle = 0; toggle("ctl"); }
                        else if (cmd.equals("on")) setForced(true, "ctl");
                        else if (cmd.equals("off")) setForced(false, "ctl");
                        else if (cmd.equals("enable")) { enabled = true; log("double-tap hook enabled"); }
                        else if (cmd.equals("disable")) { enabled = false; log("double-tap hook disabled"); }
                    }
                } catch (Throwable e) {
                    sleep(2000);
                }
            }
        }, "ctl");
        t.setDaemon(true);
        t.start();
    }

    static void loadConfig(File f) {
        if (!f.exists()) return;
        Properties p = new Properties();
        try (FileInputStream in = new FileInputStream(f)) {
            p.load(in);
            onAlpha = Float.parseFloat(p.getProperty("ALPHA", "1.0").trim());
            onZ = Integer.parseInt(p.getProperty("Z", "9999").trim());
            debounceMs = Long.parseLong(p.getProperty("DEBOUNCE_MS", "600").trim());
            enabled = !"0".equals(p.getProperty("ENABLED", "1").trim());
            muteToast = !"0".equals(p.getProperty("MUTE_SYSUI_TOAST", "1").trim());
        } catch (Throwable t) {
            log("config error: " + t);
        }
    }

    static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException ignored) { }
    }

    static void log(String s) {
        try {
            Class.forName("android.util.Log").getMethod("i", String.class, String.class).invoke(null, TAG, s);
        } catch (Throwable ignored) { }
        System.out.println(s);
    }
}
