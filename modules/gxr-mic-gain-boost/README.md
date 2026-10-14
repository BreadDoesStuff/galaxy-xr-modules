# Galaxy XR Mic Gain Boost (`gxr-mic-gain-boost`)

Boosts the headset microphone by **+15 dB** — but only while **Virtual Desktop** (**VOICE_PERFORMANCE**) is recording. Event-driven (no polling), no system-file edits, and it leaves screen recording and every other app untouched.

## Why this exists

Virtual Desktop (VD) records with source **VOICE_PERFORMANCE** (10) and the screen recorder uses **MIC**, but both go through the same mic path (**handset-mic → TX_DEC0**). When they run at the same time they also share one input stream — so editing ``mixer_paths.xml`` to boost the mic boosted *every* app. This module is scoped to VD's recording session instead.

## How it works

A small root background process waits for Android's recording start/stop notifications (no polling):

- When a **VOICE_PERFORMANCE** recording starts, it adds ``GAIN_DB`` to the current **TX_DEC0** volume (``84 → 99`` by default).
- When the recording stops, it restores the original value — but only if the audio driver hasn't already reset it.
- It saves its state to a file, so if it restarts mid-session it won't add the boost twice.
- It only uses CPU when a recording starts or stops, but it sits at about **36 MB of RAM** all the time.

## What didn't work

I first tried a +15 dB software capture effect attached only to **VOICE_PERFORMANCE** recordings. It attached to VD's session correctly, but the Samsung/Qualcomm audio driver ignores any capture effect other than echo cancellation and noise suppression, so it never processed any audio. I removed it in favor of the event-driven daemon above.

## Configuration

Config file: ``/data/adb/modules/gxr-mic-gain-boost/config.conf``
Apply changes by **rebooting** or running ``pkill -f gxr_micd``.

| Key | Default | Description |
| --- | --- | --- |
| ``MODE`` | ``1`` | When to apply the boost. See **MODE values** below. |
| ``GAIN_DB`` | ``15`` | Gain (dB) added on top of the current **TX_DEC0** volume. |

### MODE values

| Value | Behavior |
| --- | --- |
| ``1`` (default) | Boost whenever VD records. A screen recording made while VD's mic is live also gets the boost, because they share the same mic stream. |
| ``2`` | Boost only while VD is the *only* app recording the mic. |

## Verified on the headset

| Scenario | TX_DEC0 |
| --- | --- |
| VD only (mode 1) | **99** (boosted) |
| Screen recording only | 84 (stock) |
| VD + screen recording (mode 2) | 84 (stock) |
| Screen recording stopped, VD still on (mode 2) | **99** (boosted) |
| Background process restarted mid-session | **99** (boost not doubled) |
