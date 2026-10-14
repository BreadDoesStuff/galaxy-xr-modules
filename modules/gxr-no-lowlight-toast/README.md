# Galaxy XR No Low-Light Toast (`gxr-no-lowlight-toast`)

Disables the XrSystemUI ambient light sensor pipeline so the **"Brighten your area for better performance."** low-light popup never appears. No system files are modified — removing the module fully reverts.

## Why this exists

The popup is a plain `android.widget.Toast` shown by ``com.android.systemui.xr.poorconditions.PoorConditionsController`` (in ``/system_ext/priv-app/XrSystemUI/XrSystemUI.apk``). It fires from a Samsung/AndroidXR-specific sensor pipeline:

- ``LightSensorManager`` listens on the headset's ambient light sensor (``com.samsung.sensor.bio_alc``, "STK6D2X Rear ALS", sensor type ``65577``).
- It keeps a moving average over ``20`` samples; ``lowLight`` is set when the filtered lux is ``<= 3.0`` (debounced ``3000`` ms).
- ``PoorConditionsController`` combines that with *screen on* and *obstructive experience showing*, debounces it again, and shows the toast on the first low-light transition.

Every knob is read once from sysprops at ``LightSensorManager`` class init, so the only clean way to stop it without patching SystemUI is to disable the sensor pipeline itself.

## How it works

The module just sets one sysprop, in two places:

- ``system.prop``: ``ix.sysui.light_sensor.enabled=false`` (applied by KernelSU at boot, before zygote).
- ``post-fs-data.sh``: ``resetprop -n ix.sysui.light_sensor.enabled false`` — redundant safety so the prop is guaranteed set before SystemUI starts.

With the prop false, ``LightSensorManager`` never registers the ALS listener, so the low-light condition can never become true and the toast never fires. The prop is **non-persistent**, so uninstalling the module (and rebooting) restores stock behavior.

## Side effects

- ``EyesMonitor`` (the inactivity auto-sleep dialog, only armed in dim light while the headset is on your head) also reads this ALS pipeline, so it will no longer trigger in the dark.
- The tracking-loss dialogs (``Unable to detect surroundings`` etc., sourced from perception via ``PoorConditionsDispatcher``) are a **separate** system and are intentionally **not** disabled — they exist for safety.

## Verified on the headset

| Check | Before | After reboot with module |
| --- | --- | --- |
| ``getprop ix.sysui.light_sensor.enabled`` | ``true`` | **``false``** |
| ``dumpsys`` → ``LightSensorManager`` | ``lightSensorEnabled=true`` | **``lightSensorEnabled=false``**, ``history=[]`` |
| Logcat | ``PoorConditionsController: Showing dim light toast`` | none |
