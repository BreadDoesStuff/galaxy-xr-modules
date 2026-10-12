#!/system/bin/sh
# Direct deployment (bypasses `ksud module install`). Usage: sh deploy.sh <module.zip>
ZIP=$1
DEST=/data/adb/modules/gxr-mic-gain-boost
STAGE=$DEST.new
BB=/data/adb/ksu/bin/busybox
abort() { echo "$1"; rm -rf "$STAGE"; exit 1; }
ui_print() { echo "$1"; }
set_perm() { chown $2:$3 "$1"; chmod $4 "$1"; [ -n "$5" ] && chcon "$5" "$1"; }
[ -f "$ZIP" ] || abort "! zip not found"
rm -rf "$STAGE"; mkdir -p "$STAGE"
$BB unzip -oq "$ZIP" -d "$STAGE" || abort "! unzip failed"
# Patch from the live file (bind-mounted copy is handled via tag), then swap in.
MODPATH=$STAGE
. "$STAGE/customize.sh"
rm -rf "$DEST"; mv "$STAGE" "$DEST" || abort "! install failed"
chown -R 0:0 "$DEST"; chmod 0755 "$DEST"
chmod 0755 "$DEST/post-fs-data.sh"
chcon u:object_r:vendor_configs_file:s0 "$DEST/mixer_paths.xml"
echo "- deployed to $DEST (reboot to apply)"
