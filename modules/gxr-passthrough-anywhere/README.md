# Galaxy XR Passthrough Anywhere (`gxr-passthrough-anywhere`)

Uses the headset's physical touchpad **double-tap** to toggle passthrough inside **any** XR app (full-space/OpenXR) without minimizing it — double-tap again to return to the app. Driven by a root daemon; no system files modified.

## Why this exists

The touchpad double-tap is handled by **XrSystemUI** (``com.android.systemui``). While an app runs in **full-space** mode (``FULL_SPACE_UNMANAGED``, i.e. OpenXR/immersive apps like Virtual Desktop), the tap is explicitly rejected — SysUI logs ``On-device touchpad double tap request rejected in full space mode.`` and shows the toast **"Can't turn Virtual Environment on or off right now."** So exactly the apps where you'd want a quick look at the real world have no way to get one without leaving the app.

## How it works

A small root daemon (``ptd.dex``, nice-name ``gxr_ptd``) started by ``service.sh``:

1. Tails SysUI's own logcat for the double-tap rejection (``OnDeviceTouchpadInputEventReceiver``). This reuses the headset's native double-tap detector, so the user's "passthrough toggle" setting is respected and no input device is grabbed (long-press = **recenter**, single tap = cursor click, both stay untouched).
2. On a rejection it forces the compositor's global passthrough node (``android.graphics.CPM.getPassthroughNode()``) **above the app**: ``setAlpha(ALPHA).setZ(Z).setVisible(true)`` — default ``ALPHA=1.0``, ``Z=9999`` (same z-order as Home/lockscreen passthrough). Double-tap again restores the last state SysUI itself requested.
3. If SysUI pushes its own passthrough state (app exit, lockscreen, Home toggle), the daemon steps back and hands the node over.
4. A ``TaskStackListener`` tracks full-space focus:
   - Entering full-space: SysUI toasts are muted (``TOAST_WINDOW`` appop → ``ignore`` for ``com.android.systemui``).
   - Leaving full-space while passthrough is forced: passthrough is restored, and toasts are re-allowed only after SysUI's toast queue has drained (each rejected tap enqueues one ~4.1 s toast that would otherwise pop up at Home).
5. Manual control: the KernelSU **Action** button writes ``toggle`` to the module's ``ctl`` FIFO; ``on``, ``off``, ``enable``, ``disable`` are accepted as well.

The XR app keeps running the whole time — the passthrough layer is simply composited on top of it.

## Configuration

Config file: ``/data/adb/modules/gxr-passthrough-anywhere/config.conf`` (read at daemon start — **reboot** or restart the daemon to apply):

| Key | Default | Description |
| --- | --- | --- |
| ``ALPHA`` | ``1.0`` | Passthrough opacity when forced on (``0.0–1.0``). Values ``< 1.0`` blend the app over the room. |
| ``Z`` | ``9999`` | CPM z-order of the passthrough layer (``9999`` = above apps, same as Home/lockscreen passthrough). |
| ``DEBOUNCE_MS`` | ``600`` | Minimum ms between toggles. |
| ``ENABLED`` | ``1`` | ``1`` = react to double-tap in full-space apps; ``0`` = off (Action button still works). |
| ``MUTE_SYSUI_TOAST`` | ``1`` | ``1`` = hide XrSystemUI toasts (e.g. "Can't turn Virtual Environment on or off right now") only while a full-space app is focused. |

## Side effects

- **All XrSystemUI toasts are hidden while a full-space app is focused**, not just the passthrough-rejection toast. Toasts are re-allowed when the app leaves the foreground (delayed until the rejection toast queue drains), at daemon start, and on uninstall.
- While forced on, the passthrough camera runs and the **red privacy LED** lights on — the same as any native passthrough.

## Verified on the headset

Verified live with **Virtual Desktop** (``virtualdesktop.android``) in ``full-space-unmanaged`` mode on a Galaxy XR SM-I610 (Android XR 14, KernelSU):

| Check | Result |
| --- | --- |
| Double-tap with app running | Passthrough **on**, app **not minimized** |
| Double-tap again | Passthrough **off**, app untouched |
| Exit app while forced on | Passthrough restored to SysUI's state |
| Native double-tap on Home | Unaffected |
| Rejection toast in-app / delayed toast at Home | None (queue drains before toasts re-allowed) |
| Reboot | Daemon auto-starts |
