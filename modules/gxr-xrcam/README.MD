# Galaxy XR Camera Debug WebStream (`gxr-xrcam`)

Streams all **13 physical cameras** of the Galaxy XR headset as MJPEG over HTTP:
`http://<headset-ip>:8080/`. All 13 streams run simultaneously at the sensors' native rates:
**RGB ~65-68 fps, tracking 60 fps, eye 30 fps, ToF 10 fps**. Starts at boot, no system files are modified.

## Why this exists

The Galaxy XR (`SM-I610`, platform `anorak`) exposes only **4 camera IDs** to the Android camera
service: the two RGB passthrough cameras, a derived depth camera, and the virtual `avatar` camera.
The **tracking, face-tracking, ctrl-tracking, eye-tracking, and dToF depth sensors are invisible** to
the normal camera stack, and the V4L2 nodes are owned by the camera provider process, so there is no
direct capture path. This module attaches a **second, parallel QVR camera client** alongside the
system's own tracking stack, giving direct access to every physical sensor for debugging and
observation.

## How it works

- A native arm64 daemon (`bin/xrcamd`) runs as root in the KernelSU domain via `service.sh`, with
  `LD_LIBRARY_PATH=/vendor/lib64` so the vendor client lib can be dlopened.
- It drives the reverse-engineered ABI of `/vendor/lib64/libqvrcamera_client.qti.so`
  (client **v9**, device **v10**; op tables recovered from the lib's minidebuginfo symbols).
- **One worker thread per QVR device**, started on demand when the first viewer connects and stopped
  after `IDLE_MS` (default **5 s**) so cameras are not held open needlessly. `tracking` and
  `face-tracking` are sync slaves of `ctrl-tracking`, so their workers take a reference on it.
- Per-frame loop: `GetCurrentFrameNumber` -> blocking `GetFrame` -> `FrameToHardwareBuffer` ->
  convert (CPU lock for RAW8/Y16, GPU `GL_TEXTURE_EXTERNAL_OES` + FBO readback for **UBWC NV12**
  RGB) -> JPEG compress (`AndroidBitmap_compress`) -> `ReleaseFrame` -> publish to subscribers.
- Served over HTTP on `[::]:<port>` (default **8080**, dual-stack):
  - `/` — grid page of all streams over a **single `/mux` connection** (browsers cap HTTP/1.1 at 6
    connections per host, so 13 separate MJPEG connections stall). One canvas per camera, stale
    frames are dropped, fps-cap selector (5/15/30/max, default **30**), pauses while the tab is hidden.
  - `/mux?ids=<ids>&fps=<n>` — multiple streams on one connection (`u8 index, u32 LE length, JPEG`,
    index **255** = keepalive every 2 s; only the newest frame per stream is sent).
  - `/api/cameras` — JSON camera list.
  - `/stream/<id>` — MJPEG (`?fps=<n>` caps the rate).
  - `/snapshot/<id>` — single JPEG.
  - `/api/exposure?dev=<qvr-device>&ns=<ns>&gain=<g>` — manual exposure.
  - `/api/boost?id=<id>&gain=<n>` — digital gain for mono streams (viewing aid only).

### Cameras streamed

| stream id | sensor | QVR device | role | output |
|---|---|---|---|---|
| `rgb-left` | imx564_vst_left | `rgb` [0] | passthrough RGB L | 1500x1500 YUV420 UBWC @72 |
| `rgb-right` | imx564_vst_right | `rgb` [1] | passthrough RGB R | 1500x1500 YUV420 UBWC @72 |
| `tracking-left` | s5k931_hat_ld | `tracking` [0] | head tracking | 640x640 Y8 @60 |
| `tracking-right` | s5k931_hat_rd | `tracking` [1] | head tracking | 640x640 Y8 @60 |
| `face-tracking-left` | s5k931_hat_lu | `face-tracking` [0] | hand-down/face | 640x640 Y8 @60 |
| `face-tracking-right` | s5k931_hat_ru | `face-tracking` [1] | hand-down/face | 640x640 Y8 @60 |
| `ctrl-tracking-left` | s5k931_het_lu | `ctrl-tracking` [0] | hand/controller (IR torch) | 640x640 Y8 @60 |
| `ctrl-tracking-right` | s5k931_het_ru | `ctrl-tracking` [1] | hand/controller (IR torch) | 640x640 Y8 @60 |
| `eye-left-a` | s5k931_et_ld | `eye-tracking-left` [0] | eye tracking | 640x640 Y8 @30 |
| `eye-left-b` | s5k931_et_lu | `eye-tracking-left` [1] | eye tracking | 640x640 Y8 @30 |
| `eye-right-a` | s5k931_et_rd | `eye-tracking-right` [0] | eye tracking | 640x640 Y8 @30 |
| `eye-right-b` | s5k931_et_ru | `eye-tracking-right` [1] | eye tracking | 640x640 Y8 @30 |
| `depth-tof` | imx611_tof_hwid19 | `d-depth` [0] | dToF depth | 24x120 int16 raw @10 |

The `d-depth` ToF stream is **5 stacked 24x24 planes** of raw IMX611 output shown side by side, each
normalised and colour-mapped separately — it is **not** a calibrated depth map.

### Configuration

- Config: `/data/adb/modules/gxr-xrcam/xrcam.conf` — `PORT`, `RGB_SCALE` (default **2** -> 750x750 per
  eye; **1** = full 1500x1500), `QUALITY`, `IDLE_MS`.
- Log: `/data/local/tmp/xrcamd.log`.
- Install: `xrcam-kernelsu.zip` via `ksud module install <zip>` or KernelSU Manager, then reboot.
- Build: `build.ps1` (NDK r27c, target API **34**).

## Side effects

- `/api/exposure` writes exposure **shared with the system tracking stack** — use it carefully.
- `ctrl-tracking` runs a short exposure with an **IR torch on alternate frames** (torch-sequence),
  so it looks dark without hand illumination; AE on the other tracking pairs converges after ~1 s.
- Eye-tracking cameras show the **IR-lit inside of the lenses** when the headset is not worn.
- With all 13 streams open, measured LAN traffic is about **20 MB/s** (RGB ~50 KB/frame, tracking
  ~33 KB, eye ~13 KB, ToF ~16 KB). Use `?fps=` or a higher `RGB_SCALE` to reduce it.
- When the headset sleeps, the perception stack stops the cameras; the daemon re-attaches them on
  demand (verified with the display asleep).
- Nothing is mounted over `/system` or `/vendor` (`mount=false`); only files under
  `/data/adb/modules/gxr-xrcam/` are added.

## Verified on the headset

Verified on a Samsung Galaxy XR `SM-I610` (`xrvst2`/`moohan`, build `I610UEU2AZD8`, kernel 5.10.240,
`ro.boot.revision=24`) with KernelSU.

| Check | Result |
|---|---|
| All 13 streams simultaneously | **Pass** — RGB ~65-68 fps, tracking 60 fps, eye 30 fps, ToF 10 fps |
| Grid page (`/`) at `http://<headset-ip>:8080/` | **Pass** — all 13 cameras live in one browser tab |
| `/mux` at warm `fps=30` | **Pass** — every camera hit its cap (ToF at native 10 fps), ~10 MB/s |
| `/mux` at `fps=0` (max) | **Pass** — all native rates, ~21 MB/s, no corrupt frames |
| Auto-restart after sleep | **Pass** — cameras re-attach on demand with display asleep |
| Boot persistence | **Pass** — `service.sh` starts the daemon at boot and restarts it if it dies |
