#!/system/bin/sh
DATA=/data/adb/gxr-fanctl
[ -f $DATA/fand.pid ] && kill "$(cat $DATA/fand.pid)" 2>/dev/null
# hand node back to SSRM
chown 1000:1000 /sys/devices/virtual/sec_fan/fan/step 2>/dev/null
chmod 0200 /sys/devices/virtual/sec_fan/fan/step 2>/dev/null
rm -rf $DATA
