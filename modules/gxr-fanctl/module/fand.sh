#!/system/bin/sh
# gxr-fanctl daemon - drives /sys/class/sec_fan/fan/step from config.
# Runs as root (KernelSU, u:r:ksu:s0).

DATA=/data/adb/gxr-fanctl
CONF=$DATA/config.conf
STATUS=$DATA/status
PIDF=$DATA/fand.pid
LOG=$DATA/fand.log
NODE=/sys/devices/virtual/sec_fan/fan/step   # == /sys/class/sec_fan/fan/step
SYSTEM_UID=1000

mkdir -p $DATA
[ -f $PIDF ] && kill -0 "$(cat $PIDF)" 2>/dev/null && { echo "already running"; exit 0; }
echo $$ > $PIDF
: > $LOG
log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> $LOG; }

[ -e $NODE ] || { log "fan node $NODE missing, exiting"; rm -f $PIDF; exit 1; }

# ---- SSRM lock / release (runtime sysfs perms only; reset on reboot) ----
lock_node() {
    [ "$(stat -c %u $NODE)" = "0" ] && return
    chown 0:0 $NODE; chmod 0200 $NODE; log "locked step node (SSRM blocked)"
}
release_node() {
    [ "$(stat -c %u $NODE)" = "$SYSTEM_UID" ] && return
    chown $SYSTEM_UID:$SYSTEM_UID $NODE; chmod 0200 $NODE; log "released step node to SSRM"
}

cleanup() { release_node; rm -f $PIDF; echo "mode=stopped" > $STATUS; exit 0; }
trap cleanup TERM INT HUP

# ---- config ----
load_conf() {
    MODE=curve; FIXED_STEP=7; CURVE="36:2 40:3 43:4 46:5 50:6 54:7"; MIN_STEP=1
    HYST=2; INTERVAL=2; SENSORS="cpu-* cpuss-* gpuss-* ddr sys-therm-0"; FAILSAFE_STEP=7
    [ -f $CONF ] && . $CONF
    case "$INTERVAL" in ''|*[!0-9]*|0) INTERVAL=2;; esac
    case "$HYST" in ''|*[!0-9]*) HYST=2;; esac
    resolve_zones
    log "config loaded: MODE=$MODE FIXED=$FIXED_STEP CURVE='$CURVE' MIN=$MIN_STEP HYST=$HYST INT=$INTERVAL"
}
clamp_step() { case "$1" in [0-7]) echo $1;; *) echo 7;; esac; }

resolve_zones() {
    ZONES=""
    for z in /sys/class/thermal/thermal_zone*; do
        t=$(cat $z/type 2>/dev/null) || continue
        for pat in $SENSORS; do
            case "$t" in $pat)
                v=$(cat $z/temp 2>/dev/null) || break
                case "$v" in ''|*[!0-9-]*) break;; esac
                ZONES="$ZONES $z"; break;;
            esac
        done
    done
}

# Sets TMAX (milli-C) and TSRC; returns 1 if nothing readable.
read_temp() {
    TMAX=-999999; TSRC=none
    for z in $ZONES; do
        v=$(cat $z/temp 2>/dev/null) || continue
        case "$v" in ''|*[!0-9-]*) continue;; esac
        [ "$v" -lt -20000 ] || [ "$v" -gt 150000 ] && continue
        [ "$v" -gt "$TMAX" ] && { TMAX=$v; TSRC=$z; }
    done
    [ "$TSRC" != none ]
}

# step_for <milliC>
step_for() {
    s=$MIN_STEP
    for p in $CURVE; do
        th=${p%%:*}; st=${p##*:}
        [ "$1" -ge $((th * 1000)) ] && s=$st
    done
    [ "$s" -lt "$MIN_STEP" ] && s=$MIN_STEP
    clamp_step $s
}

write_step() {
    echo $1 > $NODE 2>/dev/null || { log "write step $1 failed"; return; }
    CUR=$1; LAST_WRITE=$NOW
}

CUR=-1; LAST_WRITE=0; CONF_M=""
log "fand start pid $$"
while :; do
    m=$(stat -c '%Y.%s.%i' $CONF 2>/dev/null)
    [ "$m" != "$CONF_M" ] && { CONF_M=$m; load_conf; CUR=-1; }
    NOW=$(date +%s)
    [ "$(stat -c %s $LOG 2>/dev/null || echo 0)" -gt 131072 ] && : > $LOG
    TMAX=""; TSRC=""; HAVE_T=0
    if read_temp; then HAVE_T=1; else TMAX=""; TSRC=""; fi

    case "$MODE" in
    stock)
        release_node
        echo "mode=stock step=ssrm temp=${TMAX:--} src=$(cat $TSRC/type 2>/dev/null || echo -) ts=$NOW" > $STATUS
        sleep $INTERVAL; continue;;
    max)   TARGET=7;;
    fixed) TARGET=$(clamp_step $FIXED_STEP);;
    *)     if [ $HAVE_T = 1 ]; then
               TARGET=$(step_for $TMAX)
               if [ "$CUR" -ge 0 ] && [ "$TARGET" -lt "$CUR" ]; then
                   d=$(step_for $((TMAX + HYST * 1000)))
                   [ "$d" -lt "$CUR" ] && TARGET=$d || TARGET=$CUR
               fi
           else
               TARGET=$(clamp_step $FAILSAFE_STEP)
           fi;;
    esac

    lock_node
    # write on change, or every 30s as a refresh
    if [ "$TARGET" != "$CUR" ] || [ $((NOW - LAST_WRITE)) -ge 30 ]; then
        [ "$TARGET" != "$CUR" ] && log "step $CUR -> $TARGET (T=${TMAX:-?} ${TSRC##*/})"
        write_step $TARGET
    fi
    echo "mode=$MODE step=$CUR temp=${TMAX:--} src=$(cat $TSRC/type 2>/dev/null || echo -) ts=$NOW" > $STATUS
    sleep $INTERVAL
done
