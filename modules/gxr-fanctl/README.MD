# Galaxy XR Fan Control (`gxr-fanctl`)

Overrides the system fan speed — **default: max speed**. A root daemon takes the fan away from Samsung's SSRM and drives it from a config file, with a built-in WebUI in the KernelSU manager to change everything live. No system files are modified — only a runtime permission change on one sysfs node, which resets on reboot anyway.

## Why this exists

By default the fan is controlled by Samsung's **SSRM** (System Resource Manager, `ssrm.jar` inside `system_server`), which rewrites `/sys/class/sec_fan/fan/step` about every second from its own fixed curve. There is no user-facing way to change it, and under sustained load (e.g. PCVR) the stock curve leaves the headset running hot.

## How it works

The headset has two fans on a **MAX31760** controller behind the `sec_fan` driver. The only control input is the `step` node (**0–7**, where 0 is off and 7 is the driver maximum, ~5500 RPM), mapped to PWM by the device tree: `0, 85, 90, 95, 100, 110, 120, 140`.

- After boot the daemon `chown`s the step node to root so SSRM gets `EACCES` and stops writing it (SSRM just logs it and continues; the permission resets on reboot).
- The daemon then writes the step from config, only when it changes (plus a 30 s refresh to keep `dmesg` quiet), and re-locks the node if something changes its owner back.
- The config is re-read live, so no reboot is needed for changes.
- `MODE=stock` hands the node back to SSRM and factory behaviour resumes immediately.
- Uninstall does the same hand-back to SSRM and removes `/data/adb/gxr-fanctl/`.

## Configuration

WebUI in the KernelSU manager, or edit `/data/adb/gxr-fanctl/config.conf` (applied live), or the CLI:

`su -c /data/adb/modules/gxr-fanctl/fanctl status | mode <m> | step <0-7> | curve "<pairs>" | set KEY VAL | get | log | start | stop | restart | reset`

| Key | Default | Description |
| --- | --- | --- |
| `MODE` | `max` | `stock` = SSRM, `max` = step 7, `fixed` = `FIXED_STEP`, `curve` = temperature curve |
| `FIXED_STEP` | `7` | Step for `fixed` mode (0–7) |
| `CURVE` | `"36:2 40:3 43:4 46:5 50:6 54:7"` | `tempC:step` pairs, ascending — highest pair at or below the current temp wins |
| `MIN_STEP` | `1` | Step used below the first curve pair |
| `HYST` | `2` | °C the temp must drop before stepping down (prevents oscillation) |
| `INTERVAL` | `2` | Poll interval in seconds |
| `SENSORS` | `"cpu-* cpuss-* gpuss-* ddr sys-therm-0"` | Thermal zone type globs; the hottest readable one is used |
| `FAILSAFE_STEP` | `7` | Step used if no sensor can be read |

## Verified on the headset

| Test | Result |
| --- | --- |
| Step 7 | pwm 140, ~5500 RPM |
| Step 0 | fan off (step 8 is rejected) |
| Curve (via `emul_temp`) | 47 °C → 5, 44.5 °C → 5 (held by hysteresis), 43.5 °C → 4, 39 °C → 3 |
| `MODE=stock` | SSRM resumes writing steps immediately |
| Reboot | daemon starts on its own, re-locks the node, mode persists |
