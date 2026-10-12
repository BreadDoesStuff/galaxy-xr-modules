#!/system/bin/sh
# Stop daemon, hand passthrough back to SysUI, and make sure SysUI toasts are allowed again.
# (KernelSU may run this early in boot, so the appops reset waits for boot completion.)
pkill -f gxr_ptd 2>/dev/null
(
  until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
  cmd appops set com.android.systemui TOAST_WINDOW allow
) < /dev/null > /dev/null 2>&1 &
