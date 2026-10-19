<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Canary - Xbox 360 Emulator</h1>

Xenia Canary is an experimental fork of the Xenia emulator. For more information, see the
[Xenia Canary wiki](https://github.com/xenia-canary/xenia-canary/wiki).

Come chat with us about **emulator-related topics** on [Discord](https://discord.gg/Q9mxZf9).
For developer chat join `#dev` but stay on topic. Lurking is not only fine, but encouraged!
Please check the [FAQ](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page before asking questions.
We've got jobs/lives/etc, so don't expect instant answers.

Discussing illegal activities will get you banned.

## Status

Buildbot | Status | Releases
-------- | ------ | --------
Canary (🪟, 🐧) | [![CI](https://github.com/xenia-canary/xenia-canary/actions/workflows/Orchestrator.yml/badge.svg?branch=canary_experimental)](https://github.com/xenia-canary/xenia-canary/actions/workflows/Orchestrator.yml/badge.svg?branch=canary_experimental) [![Codacy Badge](https://app.codacy.com/project/badge/Grade/cd506034fd8148309a45034925648499)](https://app.codacy.com/gh/xenia-canary/xenia-canary/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade) | [Latest](https://github.com/xenia-canary/xenia-canary/releases/latest) ◦ [All](https://github.com/xenia-canary/xenia-canary/releases) ◦ [Old](https://github.com/xenia-canary/xenia-canary-releases/releases)

### Experimental Netplay

Buildbot | Status | Releases
-------- | ------ | --------
Windows | [![Codacy Badge](https://app.codacy.com/project/badge/Grade/d814c4b6aa444dcc9c1631e0224b2739)](https://app.codacy.com/gh/AdrianCassar/xenia-canary/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade) | [Latest](https://github.com/AdrianCassar/xenia-canary/releases/latest)

## Quickstart

See the [Quickstart](https://github.com/xenia-canary/xenia-canary/wiki/Quickstart) page.

## FAQ

See the [frequently asked questions](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page.

## Game Compatibility

See the [Game compatibility list](https://github.com/xenia-canary/game-compatibility/issues)
for currently tracked games, and feel free to contribute your own updates,
screenshots, and information there following the [existing conventions](https://github.com/xenia-canary/game-compatibility/blob/canary/README.md).

## Building

See [building.md](docs/building.md) for setup and information about the
`xb` script. When writing code, check the [style guide](docs/style_guide.md)
and be sure to run clang-format!

## Contributors Wanted!

Have some spare time, know advanced C++, and want to write an emulator?
Contribute! There's a ton of work that needs to be done, a lot of which
is wide open greenfield fun.

**For general rules and guidelines please see [CONTRIBUTING.md](.github/CONTRIBUTING.md).**

Fixes and optimizations are always welcome (please!), but in addition to
that there are some major work areas still untouched:

* Help work through [missing functionality/bugs in games](https://github.com/xenia-canary/xenia-canary/labels/compat)
* Reduce the size of Xenia's [huge log files](https://github.com/xenia-canary/xenia-canary/issues/1526)
* Skilled with Linux? A strong contributor is needed to [help with porting](https://github.com/xenia-canary/xenia-canary/labels/platform-linux)

See more projects [good for contributors](https://github.com/xenia-canary/xenia-canary/labels/good%20first%20issue). It's a good idea to ask on Discord and check the issues page before beginning work on
something.

## Disclaimer

The goal of this project is to experiment, research, and educate on the topic
of emulation of modern devices and operating systems. **It is not for enabling
illegal activity**. All information is obtained via reverse engineering of
legally purchased devices and games and information made public on the internet
(you'd be surprised what's indexed on Google...).

## My Info 
# 🕺 Xenia Canary — Xbox 360 Kinect Integration (Dance Central)

[![Status](https://img.shields.io/badge/Status-Experimental-orange.svg)]()
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11-blue.svg)]()
[![Xenia](https://img.shields.io/badge/Emulator-Xenia%20Canary-purple.svg)]()

This repository/branch contains ongoing work to integrate physical **Xbox 360 Kinect (V1)** hardware directly into **Xenia Canary**, with a primary focus on game preservation and playability for the *Dance Central* series.

> ⚠️ **Current Project Status:** The physical sensor successfully streams depth and color data, the game detects player silhouettes, and **menus are fully navigable using hand gestures**. However, during active song gameplay, the score remains stuck at `0` and all dance moves are evaluated as failed (current primary blocker).

---

## 📌 TL;DR / Quick Summary

* **What works:**
  * Direct USB communication using `UsbDk`.
  * Stable Depth + Color streaming at 30 FPS (~6,100 frames in continuous stress tests).
  * Player body detection (silhouette rendering) and proximity alerts (framing warnings).
  * Full menu navigation using hand gestures.
  * Solid performance (40–60 FPS during gameplay).
* **What doesn't work yet:**
  * **Gameplay scoring:** Move evaluation fails during songs (all move feedback flashes red).
  * **Freestyle Photo:** Captures a pitch-black image due to a color format mismatch (Bayer vs. YUV).
  * **Dance Central 2:** Currently hangs on a black screen on startup.
* **Codebase Origin:** Based on `@knuckleslee`'s `kinect-v1-stage1` branch (commit `2a5ae4c43`).

---

## 🏗️ Architecture: How Data Flows

Unlike alternative approaches that attempt to compute skeletal tracking on the host PC using external libraries, **this implementation feeds raw sensor streams directly to the emulated game environment**. The game's embedded Xbox 360 Kinect NUI library processes the skeletal tracking internally, exactly as original hardware did.

```text
  [ Kinect Physical Hardware (USB) ]
                  │
                  ▼
         [ UsbDk / libusb ]
                  │
                  ▼
  [ Xenia Host (PsCamDeviceRequest) ]  <─── Captures Depth/Color (30 fps)
                  │
                  ▼
  [ Guest Emulated System Memory ]
                  │
                  ▼
 [ In-Game Embedded NUI Library ]     <─── (Calculates skeleton using GPU Readbacks)
                  │
                  ▼
 [ Dance Central: Menus / Gameplay ]   <─── Hand Cursor ✅ / Gameplay Scoring ❌
```

---

## 🚀 Quick Setup Guide

### 1. Hardware & Driver Requirements

* **Sensor:** Xbox 360 Kinect (`VID_045E&PID_02AE`).
* **USB Driver:** You must install [UsbDk](https://github.com/daynix/UsbDk) (do **not** use the official Microsoft Kinect SDK or raw `libfreenect` drivers for the USB port):

```powershell
winget install --id daynix.UsbDk --exact
```

* **Recommended Placement:** Mount the sensor level at 0° elevation, at hip height, with the player standing at least 3 meters (10 ft) back for full-body tracking.

### 2. `xenia-canary.config.toml` Configuration

Ensure the following keys are set in your `.config.toml` file:

```toml
[HID]
allow_kinect_camera = true
kinect_use_usbdk = true

[GPU]
# BOTH readbacks are strictly required for body tracking
readback_resolve = "fast"
readback_memexport = true

[Memory]
protect_zero = false
```

### 3. Emulator UI Settings

1. Open Xenia and navigate to the **Console** menu.
2. Check the **Kinect Initialized** option.
3. Sign into a local user profile in Xenia before launching the game.

---

## 🔍 Test Matrix & Verified Results

### Game Compatibility

| Game | Title ID | Status | Details |
| :--- | :--- | :---: | :--- |
| Dance Central 1 | `545607D3` | 🟡 Partial | Menus OK. Player detection OK. Gameplay score stuck at 0. |
| Dance Central 2 | `544E0803` | 🔴 Broken | Black screen on boot (needs further isolation). |
| Dance Central 3 | `545408AE` | ⚪ Untested | Pending verification. |
| Just Dance 4 | `555308C2` | 🔴 Inactive | Game fails to wake Kinect (`XamNuiIsDeviceReady` evaluates to 0). |

### Impact of GPU Readbacks

| `readback_resolve` | `readback_memexport` | Body Detection | Performance |
| :---: | :---: | :--- | :---: |
| `fast` | `true` | ✅ Yes (Working) | 40–60 FPS |
| `full` | `true` | ✅ Yes (Working) | 40–60 FPS |
| `none` | `true` | ❌ No (Player Not Found) | 60 FPS |
| `fast` | `false` | ❌ No (Player Not Found) | 60 FPS |

> **Key Takeaway:** Internal skeleton calculation relies on GPU compute passes whose results must be flushed back to CPU-accessible guest memory (`memexport` + `resolve`).

---

## 🛠️ The Primary Blocker: Why Doesn't Scoring Work?

Although physical presence is recognized (the gray player silhouette renders, gameplay pauses if you step out of frame, and red boundary alerts trigger if you step too close), move evaluations fail constantly during dance routines.

### Current Active Hypotheses

* **H1 — Invalid/Partial Skeleton (High Priority):** Joint data in `NUI_SKELETON_FRAME` structures might be flagged as `inferred` or `not_tracked` due to minor GPU emulation inaccuracies.
* **H2 — Frozen Frame Timestamps:** The game's NUI library computes motion velocity using millisecond deltas between consecutive frames (`liTimeStamp`). Static or missing timestamps allow menu cursors to work (since positioning is static/absolute), but break fast motion tracking.
* **H3 — Color Buffer Discrepancy:** Xenia writes 307,200 bytes (8-bit raw Bayer) while the game allocates 614,400 bytes (YUV 4:2:2). While depth handles skeletal tracking, this discrepancy breaks Freestyle mode photos (black capture).

---

## 📑 Diagnostic Patches & Tooling

The following diagnostic patches are available in the working tree for debugging:

| Patch | Purpose |
| :--- | :--- |
| `kinect-diag-2b002.patch` | Handles XAM message `0x2B002` to eliminate infinite boot loops. |
| `kinect-diag-pscam.patch` | Logs depth/color buffer telemetry and frame statistics. |
| `kinect-diag-gpu.patch` | Measures GPU lock stall durations during memexport operations. |
| `kinect-diag-skeleton.patch` | In-memory scanner for real-time inspection of guest skeletal structures. |

### Helpful PowerShell Snippets

Quickly switch GPU readback modes:

```powershell
Set-Readback 'fast' 'true'
```

Filter warnings and errors from log output:

```powershell
Get-Content xenia.log | Where-Object { $_ -match '^[!w]>' } | Select-Object -Last 30
```

---

## 🗺️ Roadmap & Next Steps

- [ ] **Step 1:** Run `kinect-diag-skeleton.patch` to verify whether joint positions and frame timestamps advance dynamically during songs.
- [ ] **Step 2:** Implement Bayer 640x480 → YUV 4:2:2 color format conversion in `xboxkrnl_pscam.cc` to fix Freestyle snapshots.
- [ ] **Step 3:** Fix `XamNuiIsDeviceReady` return logic to enable sensor initialization in games like Just Dance 4.
- [ ] **Step 4:** Clean up diagnostic code and break functional additions into small, atomic Pull Requests for upstream Xenia Canary.

---

## 📚 References & Credits

* **Original PR:** [xenia-canary/#1274](https://github.com/xenia-canary/xenia-canary/pull/1274) by [@knuckleslee](https://github.com/knuckleslee).
* **Decompilation Reference:** [rjkiv/dc3-decomp](https://github.com/rjkiv/dc3-decomp) (used for reverse engineering Dance Central's NUI library integration).
* **USB Driver:** [UsbDk](https://github.com/daynix/UsbDk) by Daynix.

