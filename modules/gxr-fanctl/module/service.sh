#!/system/bin/sh
# late_start service: wait for boot + SSRM, then start the fan daemon.
MODDIR=${0%/*}
DATA=/data/adb/gxr-fanctl
mkdir -p $DATA
[ -f $DATA/config.conf ] || cp $MODDIR/config.default.conf $DATA/config.conf
rm -f $DATA/fand.pid

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
# init.samsung.motor.rc chowns the node to system "on boot"; let that finish.
sleep 10
nohup sh $MODDIR/fand.sh >/dev/null 2>&1 &
