#!/system/bin/sh
[ -x /system/bin/tinymix ] || abort "! /system/bin/tinymix not found"
tinymix -D 0 -v "TX_DEC0 Volume" >/dev/null 2>&1 || abort "! mixer control 'TX_DEC0 Volume' not found - unsupported firmware"
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/micd.dex" 0 0 0644
ui_print "- Mic boost applies only while Virtual Desktop records (see config.conf)"
