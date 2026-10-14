#!/system/bin/sh
# Starts the event-driven mic boost daemon (idle until a VOICE_PERFORMANCE recording starts).
MODDIR=${0%/*}
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
(
  while [ ! -f "$MODDIR/disable" ]; do
    CLASSPATH="$MODDIR/micd.dex" /system/bin/app_process /system/bin --nice-name=gxr_micd \
      com.gxr.mic.Main "$MODDIR" >> "$MODDIR/daemon.log" 2>&1
    [ "$(stat -c %s "$MODDIR/daemon.log" 2>/dev/null || echo 0)" -gt 65536 ] && : > "$MODDIR/daemon.log"
    sleep 3
  done
) < /dev/null > /dev/null 2>&1 &
