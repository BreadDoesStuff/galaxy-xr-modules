#!/system/bin/sh
# GXR Passthrough Anywhere - start root daemon after boot
MODDIR=${0%/*}

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
# give XrSystemUI time to come up
sleep 10

[ -p "$MODDIR/ctl" ] || { rm -f "$MODDIR/ctl"; mkfifo -m 600 "$MODDIR/ctl"; }

(
  while true; do
    [ -f "$MODDIR/disable" ] && exit 0
    CLASSPATH="$MODDIR/ptd.dex" /system/bin/app_process /system/bin --nice-name=gxr_ptd \
      com.gxr.pt.Main daemon "$MODDIR" >> "$MODDIR/daemon.log" 2>&1
    # keep log small
    [ "$(stat -c %s "$MODDIR/daemon.log" 2>/dev/null || echo 0)" -gt 262144 ] && : > "$MODDIR/daemon.log"
    sleep 3
  done
) < /dev/null > /dev/null 2>&1 &
