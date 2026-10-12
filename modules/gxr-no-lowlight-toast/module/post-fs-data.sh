#!/system/bin/sh
# Redundant with system.prop; guarantees the prop is set before zygote/SystemUI start.
resetprop -n ix.sysui.light_sensor.enabled false
