# nullmoth-nvidia-audio (`NMHDA.kext`)

**Universal HDMI & DisplayPort Audio + Native Volume & Mute Control for NVIDIA RTX GPUs on macOS Sequoia (15) & Tahoe (26)**

> [!NOTE]
> **AI-Assisted Reverse Engineering Project:**
> This project was reverse-engineered, developed, and tested with the assistance of AI (Google DeepMind Antigravity). It is shared openly to help the Hackintosh / NullMoth NVIDIA community get working multi-port HDMI/DisplayPort audio and native volume control on modern NVIDIA GPUs.

---

## Overview

While the NullMoth NVIDIA driver enables full Metal acceleration and display output on modern NVIDIA GPUs (Turing / RTX 20, Ampere / RTX 30, Ada Lovelace / RTX 40, and Blackwell / RTX 50) on macOS 15 Sequoia and macOS 26 Tahoe, the stock `NMHDA.kext` only patches the PCI device ID table in `AppleHDAController` and leaves HDMI/DisplayPort audio non-functional on modern cards.

**`nullmoth-nvidia-audio` (`NMHDA.kext`)** is an enhanced Lilu plugin that:
1. Enables **all 4 HDMI/DisplayPort audio outputs simultaneously** (`fb0..fb3`).
2. Brings **native macOS volume slider & keyboard mute/volume controls** directly to HDMI and DisplayPort monitors—without requiring third-party virtual audio apps (such as MonitorControl, EqMac, or SoundSource).
3. Works alongside onboard motherboard audio (`AppleALC.kext` + `alcid=...`) without conflict.

---

## ⚠️ Prerequisites (Important for macOS 26 Tahoe!)

1. **[NullMoth / nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)** installed and active.
2. **[Lilu.kext](https://github.com/acidanthera/Lilu/releases)** loaded in OpenCore (`EFI/OC/Kexts/`).
3. **`AppleHDA.kext` (Required on macOS 26 Tahoe):**
   - **macOS 15 Sequoia:** `AppleHDA.kext` is already built into macOS.
   - **macOS 26 Tahoe:** Apple removed `AppleHDA.kext` from macOS 26 Tahoe. You **must** install `AppleHDA.kext` back into `/System/Library/Extensions` before this kext can work.
   - Download and install `AppleHDA.kext` using **[Mirone's MyKextInstaller](https://github.com/Mirone/MyKextInstaller/releases)**.

---

## What It Fixes (Technical Summary)

1. **Dynamic NVIDIA PCI Audio ID Patching (`10DE:xxxx` & `AppleALC` Coexistence):**
   - Scans PCI (`class-code 0x040300`, `vendor-id 0x10DE`) and patches `AppleHDAController`'s Kepler lookup table in memory.
   - Also adds `10DE:9D70` when `AppleALC.kext` is present (since `AppleALC` renames non-Intel/AMD controllers during probe), allowing onboard analog audio and NVIDIA HDMI/DP audio to work together.
2. **All 4 Display Heads Mapped (`IOFBDependentIndex 0..3`):**
   - Injects `connector-type`, `av-signal-type`, and `audio-codec-info` (`(pin << 16) | (conv << 8)`) across all 4 `NVRMFramebuffer` heads (`Pin 4..7` / `Converter 8..11` in `AppleHDAFunctionGroupGK10X`).
3. **GK10X Pin Sense & EDID Handshake Fix:**
   - Forces the HDA `Presence Detect` bit (`0x80000000`) on GK10X digital pin widgets (`NID 4..7`) where modern NVIDIA GPUs return `0x0` for unsolicited pin sense.
   - Triggers `AppleHDAController::framebufferEventGate` (`event = 2`) once `readyToSendFBMsg` is active so `AppleHDAHDMI_DPDriver::handleValidEDID` registers an `AppleHDAEngineOutputDP` audio sink for every connected display.
4. **Native macOS Volume Slider & Mute Control (Kernel PCM Scaling):**
   - Hooks `AppleHDAEngineOutputDP::initializeSupportForMCCS` and invokes `AppleHDAEngineOutputDP::intialiseMCCSControls` on the engine's `IOWorkLoop` to publish native `IOAudioLevelControl` (Volume) and `IOAudioToggleControl` (Mute) controls in macOS Sound Settings and Control Center.
   - Hooks `AppleHDAEngine::clipOutputSamples` (strictly scoped to `AppleHDAEngineOutputDP` instances so onboard motherboard audio is untouched) and applies a smooth cubic perceptual volume curve in 16-bit fixed-point integer arithmetic (`16-bit` and `32-bit` LPCM streams).

---

## Installation (OpenCore)

1. Make sure `AppleHDA.kext` is installed if you are on **macOS 26 Tahoe** (via [Mirone/MyKextInstaller](https://github.com/Mirone/MyKextInstaller/releases)).
2. Download [`NMHDA.kext`](./Release/NMHDA.kext) (or [`NMHDA-v1.1.0-macOS-Sequoia-Tahoe.zip`](./Release/NMHDA-v1.1.0-macOS-Sequoia-Tahoe.zip)) from the [`Release/`](./Release) folder.
3. Mount your OpenCore `EFI` partition and copy `NMHDA.kext` into `EFI/OC/Kexts/` (overwrite the existing `NMHDA.kext` if present).
4. Ensure both `Lilu.kext` and `NMHDA.kext` are enabled in your `EFI/OC/config.plist` under `Kernel -> Add` (`Lilu.kext` must be listed before `NMHDA.kext`).
5. Reboot.

---

## Diagnostics

After booting, run the following command in Terminal to inspect the live driver log and verify detected displays and volume controls:

```bash
sysctl debug.nmhda
```

Example working output (RTX 3060 on macOS 26 Tahoe with 1x HDMI + 2x DisplayPort monitors):
```text
debug.nmhda: waiting for AppleHDA | AppleHDAController loaded (index 1) | HD Audio table: 10DE:228E now accepted (in place of Kepler 10DE:0E0A) | HD Audio table: 10DE:9D70 accepted (in place of Kepler 10DE:0E0B) | matching restarted on 1 NVIDIA audio function(s) | probe HDAU -> accepted | start HDAU -> ok | graphics link announce via GFX0 -> 0x0 | AppleHDA loaded (index 2) | gk10x nid=4 hwSense=0x0 | gk10x nid=5 hwSense=0x0 | gk10x nid=6 hwSense=0x0 | gk10x nid=7 hwSense=0x0 | fb0 props set on NVRMFramebuffer codec-info=0x040800 | fb1 props set on NVRMFramebuffer codec-info=0x050900 | fb2 props set on NVRMFramebuffer codec-info=0x060a00 | gfxMatch AppleDisplay fb0=1 | MCCS virtual volume init rc=0x0 vol=254 | handleValidEDID pin=4 conv=8 sig=0x8 -> 0x0 sink=1 | gfx[0] head=0x44377064 rdy=1 -> 0x0 sink=1 pin=4 conv=8 | gfxMatch AppleDisplay fb0=1 | MCCS virtual volume init rc=0x0 vol=254 | handleValidEDID pin=5 conv=9 sig=0x8 -> 0x0 sink=1 | gfx[1] head=0x46327064 rdy=1 -> 0x0 sink=1 pin=5 conv=9 | gfxMatch AppleDisplay fb0=1 | MCCS virtual volume init rc=0x0 vol=254 | handleValidEDID pin=6 conv=10 sig=0x8 -> 0x0 sink=1 | gfx[2] head=0x43417064 rdy=1 -> 0x0 sink=1 pin=6 conv=10
```

---

## Building from Source

```bash
git clone https://github.com/emirercan3459/nullmoth-nvidia-audio.git
cd nullmoth-nvidia-audio
bash ./build.sh
```

---

## Credits

- **[NullMoth / nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)** — Original NVIDIA macOS driver & initial `NMHDA` PCI table patcher concept
- **[Acidanthera / Lilu](https://github.com/acidanthera/Lilu)** — Kernel patching framework
- **[Mirone / MyKextInstaller](https://github.com/Mirone/MyKextInstaller/releases)** — Restoring `AppleHDA.kext` on macOS 26 Tahoe
- **emirercan3459 & Antigravity AI** — Reverse engineering `AppleHDA` / `AppleHDAController` GK10X multi-port EDID handshake and native virtual MCCS PCM volume/mute controls
