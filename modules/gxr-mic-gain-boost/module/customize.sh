#!/system/bin/sh
# Generates a patched mixer_paths.xml from the device's own copy at install time.
# Only the "handset-mic" path (PAL_DEVICE_IN_HANDSET_MIC -> DMIC1 -> TX_DEC0) is touched.
# TX_DEC0 Volume: 0..124, 84 = 0 dB, 1 dB/step.
# Applied via bind mount in post-fs-data.sh (skip_mount policy: no magic mount).

BOOST_DB=15
PATHNAME=handset-mic
CTL="TX_DEC0 Volume"
SRC=/vendor/etc/mixer_paths.xml
DST=$MODPATH/mixer_paths.xml
TAG=gxr-mic-gain-boost

[ -f "$SRC" ] || abort "! $SRC not found"

# If this module is already active, $SRC is our bind mount; recover original value from tag.
BASE=$(sed -n "s/.*<!-- $TAG orig=\([0-9]*\) .*/\1/p" "$SRC" | head -n1)

awk -v p="$PATHNAME" -v c="$CTL" -v b="$BOOST_DB" -v base="$BASE" -v tag="$TAG" '
  index($0, "<!-- " tag " ") { next }
  index($0, "<path name=\"" p "\">") { inp = 1 }
  inp && index($0, "<ctl name=\"" c "\"") {
    if (match($0, /value="[0-9]+"/)) {
      orig = (base != "") ? base : substr($0, RSTART + 7, RLENGTH - 8)
      nv = orig + b; if (nv > 124) nv = 124
      sub(/value="[0-9]+"/, "value=\"" nv "\"")
      done = 1
      print "        <!-- " tag " orig=" orig " new=" nv " -->"
    }
  }
  inp && /<\/path>/ { inp = 0 }
  { print }
  END { exit done ? 0 : 1 }
' "$SRC" > "$DST" || { rm -f "$DST"; abort "! '$CTL' not found in path '$PATHNAME' - unsupported firmware"; }

ui_print "- $(grep -o "$TAG orig=[0-9]* new=[0-9]*" "$DST")"
set_perm "$DST" 0 0 0644 u:object_r:vendor_configs_file:s0
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
