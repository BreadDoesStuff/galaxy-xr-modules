#!/system/bin/sh
# Runs before class "hal" starts, so the audio HAL parses the patched file on its first load.
MODDIR=${0%/*}
F=$MODDIR/mixer_paths.xml
T=/vendor/etc/mixer_paths.xml
[ -s "$F" ] || exit 0
chcon u:object_r:vendor_configs_file:s0 "$F"
mount -o bind "$F" "$T"
