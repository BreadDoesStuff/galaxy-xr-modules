#!/system/bin/sh
# KernelSU installer hook
SKIPUNZIP=0
if [ "$ARCH" != "arm64" ]; then abort "arm64 only"; fi
if [ ! -f /vendor/lib64/libqvrcamera_client.qti.so ]; then
    abort "! /vendor/lib64/libqvrcamera_client.qti.so not found - not a Galaxy XR / QVR device"
fi
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/xrcamd" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
ui_print "- xrcamd will listen on port 8080 after reboot"
ui_print "- open http://<headset-ip>:8080/ on your LAN"
