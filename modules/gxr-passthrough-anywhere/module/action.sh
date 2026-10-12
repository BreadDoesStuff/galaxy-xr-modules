#!/system/bin/sh
# KernelSU "Action" button: manually toggle forced passthrough
MODDIR=${0%/*}
if [ -p "$MODDIR/ctl" ] && pidof gxr_ptd > /dev/null; then
  echo toggle > "$MODDIR/ctl" &
  sleep 1
  tail -n 1 "$MODDIR/daemon.log"
else
  echo "daemon not running"
fi
