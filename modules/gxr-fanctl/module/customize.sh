#!/system/bin/sh
SKIPUNZIP=0
DATA=/data/adb/gxr-fanctl

if [ ! -e /sys/class/sec_fan/fan/step ]; then
    ui_print "! /sys/class/sec_fan/fan/step not found - unsupported device"
    abort "! Aborting"
fi
ui_print "- Fan node found: $(cat /sys/class/sec_fan/fan/get_status 2>/dev/null)"

mkdir -p $DATA
if [ -f $DATA/config.conf ]; then
    ui_print "- Keeping existing config $DATA/config.conf"
else
    cp $MODPATH/config.default.conf $DATA/config.conf
    ui_print "- Default config written to $DATA/config.conf"
fi

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/fand.sh 0 0 0755
set_perm $MODPATH/fanctl 0 0 0755
set_perm $MODPATH/service.sh 0 0 0755
set_perm $MODPATH/action.sh 0 0 0755
set_perm $MODPATH/uninstall.sh 0 0 0755
ui_print "- CLI: su -c /data/adb/modules/gxr-fanctl/fanctl"
ui_print "- WebUI available in KernelSU manager"
