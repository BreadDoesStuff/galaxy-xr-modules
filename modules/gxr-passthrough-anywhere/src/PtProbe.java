import java.lang.reflect.Method;

/** Probe: drive the global CPM passthrough node. usage: PtProbe <alpha> <z> [holdMs] */
public class PtProbe {
    public static void main(String[] a) throws Throwable {
        float alpha = Float.parseFloat(a[0]);
        int z = Integer.parseInt(a[1]);
        long hold = a.length > 2 ? Long.parseLong(a[2]) : 0;
        Class<?> cpm = Class.forName("android.graphics.CPM");
        Class<?> node = Class.forName("android.graphics.CPM$Node");
        Class<?> nu = Class.forName("android.graphics.CPM$NodeUpdates");
        Object n = cpm.getMethod("getPassthroughNode").invoke(null);
        System.out.println("node=" + n);
        Object u = nu.getConstructor().newInstance();
        nu.getMethod("setAlpha", node, float.class).invoke(u, n, alpha);
        nu.getMethod("setZ", node, int.class).invoke(u, n, z);
        nu.getMethod("setVisible", node, boolean.class).invoke(u, n, alpha > 0f);
        nu.getMethod("apply").invoke(u);
        System.out.println("applied alpha=" + alpha + " z=" + z);
        if (hold > 0) Thread.sleep(hold);
    }
}
