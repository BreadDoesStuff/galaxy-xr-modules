# Galaxy XR KernelSU Modules

KernelSU modules for the Samsung Galaxy XR (SM-I610) running AndroidXR. All modules are non-invasive — none modify system files.
Requires a rooted Samsung Galaxy XR with [KernelSU-GXR](https://github.com/4uva4ek/KernelSU-GXR).  

Add this repository as a source within KernelSU with the url: ``https://github.com/BreadDoesStuff/galaxy-xr-modules``.

## CAUTION
Modules marked with (*) are experimental and may either have negative **performance impacts** or unintended effects. Further testing is required.

That being said, Rooting your headset also carries these risks.

## Modules

### [Galaxy XR Fan Control (`gxr-fanctl`)](modules/gxr-fanctl/README.md)
Overrides the system fan speed (default: **max speed**). Includes a built-in configuration WebUI within KernelSU. You can set a custom fan step (0–7, where 0 is off and 7 is max) or a temperature-based fan speed curve.

### [Galaxy XR No Low-Light Toast (`gxr-no-lowlight-toast`)](modules/gxr-no-lowlight-toast/README.md)
Disables the ambient light sensor pipeline so the **"Brighten your area for better performance."** low-light popup never appears.

### * [Galaxy XR Passthrough Anywhere (`gxr-passthrough-anywhere`)](modules/gxr-passthrough-anywhere/README.md)
Uses the headset's physical double-tap to toggle passthrough inside **any** XR app (full-space/OpenXR) without minimizing it. Double-tap again to return to the app.
* Performance issues may occur while streaming PCVR. Further testing required. A possible refactor may be in order.

### * [Galaxy XR Camera Debug WebStream (`gxr-xrcam`)](modules/gxr-xrcam/README.md)
Streams all **13 physical headset cameras** as MJPEG over HTTP on port 8080 (`http://<headset-ip>:8080/`). All streams run simultaneously at the sensors' native rates (RGB ~65–68 fps, tracking 60 fps, eye 30 fps, ToF 10 fps). Starts at boot.
* Running camera streams may have a performance impact on the headset. Further testing required. Purely for debug purposes.

### * [Galaxy XR Mic Gain Boost (`gxr-mic-gain-boost`)](modules/gxr-mic-gain-boost/README.md)
+15 dB mic gain only while Virtual Desktop records (VOICE_PERFORMANCE source). Event-driven, no polling. Screen recording / other apps alone are untouched. MODE in config.conf.
* Needs more testing.