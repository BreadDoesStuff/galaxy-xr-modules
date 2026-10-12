#!/system/bin/sh
# KernelSU late_start service: launches xrcamd and keeps it alive.
MODDIR=${0%/*}
LOG=/data/local/tmp/xrcamd.log
CONF=$MODDIR/xrcam.conf

# defaults; override in xrcam.conf
PORT=8080
RGB_SCALE=2
QUALITY=80
IDLE_MS=5000
[ -f "$CONF" ] && . "$CONF"

# wait for boot + QVR service
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
for i in $(seq 1 60); do
    [ -n "$(pidof qvrservice)" ] && break
    sleep 2
done
sleep 5

chmod 0755 "$MODDIR/bin/xrcamd"
(
    while true; do
        echo "--- xrcamd start $(date)" >> "$LOG"
        LD_LIBRARY_PATH=/vendor/lib64 "$MODDIR/bin/xrcamd" -p "$PORT" -s "$RGB_SCALE" -q "$QUALITY" -i "$IDLE_MS" >> "$LOG" 2>&1
        echo "--- xrcamd exited $? $(date)" >> "$LOG"
        # keep the log bounded
        [ "$(stat -c %s "$LOG" 2>/dev/null || echo 0)" -gt 4000000 ] && mv -f "$LOG" "$LOG.old"
        sleep 3
    done
) &
