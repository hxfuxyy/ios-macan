<p align="center">
  <img src="uefi/branding/assets/logo.png" width="120" alt="HackIOS">
</p>

<h1 align="center">ios-macan</h1>

<p align="center"><strong>Real iOS 16.5 on the OnePlus Ace 6T</strong><br>
Apple's own XNU kernel and iOS userland, booted straight from RAM</p>

<p align="center">No emulator &nbsp;·&nbsp; nothing flashed &nbsp;·&nbsp; one reboot and Android is back</p>

---

## What is this

`ios-macan` runs Apple's XNU kernel and the iOS 16.5 userland (the iPhone X build) on the **OnePlus Ace 6T**
(PLR110, Snapdragon SM8845, Adreno 829). Not a skin, not a launcher: SpringBoard, Settings, apps, Sileo and
tweaks run on the phone's own CPU and GPU.

Nothing is written to the phone: everything goes over USB into RAM, and after a reboot Android is exactly as it
was. iOS itself is not in this repository; the script downloads it from Apple's servers.

## Quick start

You need a **OnePlus Ace 6T with an unlocked bootloader** and USB debugging on, and a **Linux PC** with Python 3.8+
and about 15 GB of free space.

```sh
git clone https://github.com/hxfuxyy/ios-macan.git && cd ios-macan
./hackios --wifi "Your Wi-Fi" "your-password"   # the network iOS joins by itself
./hackios --get                                 # once: the release kit, iOS 16.5 from Apple, the images
./hackios --boot                                # the phone goes to the bootloader, then into iOS
```

- iOS starts on the lock screen: swipe up.
- Wi-Fi: iOS joins the network given with `--wifi` by itself. Joining from Settings does not work yet.
  To change the network later run `--wifi` again (it takes a few seconds), `--wifi off` for none.
- Back to Android: hold the power button until the phone restarts.
- `--get` shows progress for every download; if it stops, run it again and it continues where it was.

## What works

| | Status | Notes |
| :--- | :---: | :--- |
| **CPU, interrupts, timers** | ✅ | all 8 cores, six at full clock |
| **Display** | ✅ | iPhone X layout (375 × 812 points) |
| **GPU** | ✅ | OpenGL ES through Mesa on the Adreno 829; the whole interface is GPU-drawn, 80–100 fps |
| **Metal** | ❌ | apps that need Metal do not run |
| **Touch** | ✅ | multi-touch |
| **Buttons** | ✅ | power, volume up, volume down |
| **Wi-Fi** | ⚠️ | the network given with `--wifi`: DHCP, Internet; joining from Settings not yet |
| **Vibration** | ✅ | keyboard and system haptics, vibrations, CoreHaptics |
| **Flashlight** | ✅ | from Control Center, with brightness levels |
| **Battery** | ⚠️ | real level, voltage, temperature and cycles; iOS does not manage charging |
| **Packages** | ✅ | Sileo, ElleKit, PreferenceLoader, tweaks, NewTerm |
| **Dynamic Island** | ⚠️ | experimental (`--island 2`): iOS's own alerts show in it, live activities such as timers not yet |
| **120 Hz** | ⚠️ | iOS draws above 60 fps; the panel itself still runs its default mode |
| **Sound** | ❌ | the audio DSP is being brought up |
| **Microphone** | ❌ | waits on the audio DSP |
| **Camera** | ❌ | |
| **Sensors** | ❌ | gyroscope, accelerometer, proximity, light |
| **Brightness** | ❌ | one fixed level |
| **Bluetooth** | ❌ | |
| **Cellular, SIM** | ❌ | |
| **GPS, NFC** | ❌ | |
| **USB** | ⚠️ | only the link to the PC that boots iOS and fetches logs; no USB inside iOS |
| **Storage (UFS)** | ❌ | iOS runs from RAM: whatever you install is gone after a reboot |
| **Sleep** | ❌ | the phone does not suspend while iOS runs |
| **Hardware AES** | ❌ | encryption runs in software |
| **Apple ID, App Store downloads** | ❌ | the App Store opens, but signing in needs a Secure Enclave the phone does not have |

## If something goes wrong

- **"no phone over USB"**: check the cable and that USB debugging is on; allow the PC on the phone.
- **"no permissions"**: `./hackios --udev` once, then plug the phone in again.
- **Black screen or stuck on the logo**: restart the phone into Android and run `--boot` again.
- **Wrong time zone in iOS**: it is taken from your PC; set another with `TZ_NAME=Europe/Berlin ./hackios --assemble`.
- **Anything else**: the message says what to do; unexpected errors go to `work/logs/hackios.log`.

## How it boots

```text
Android ─▶ bootloader ── fastboot boot (RAM) ──▶ UEFI (uefi/) ──▶ USB loader (loader/)
        ──▶ XNU 8796.121.2 + kernel/ ──▶ root image + iOS system image in RAM ──▶ launchd ──▶ SpringBoard
```

## Inside

| | |
| :--- | :--- |
| `hackios` | the one entry point |
| `kernel/` | `kernel.patch` (Apple's files) and `files/` (the phone's drivers, GPU, packfs, IOKit services) |
| `loader/` | the USB loader that takes the kernel and the images |
| `uefi/` | the complete UEFI firmware source (Project Mu with the Ace 6T platform, menu and branding) |
| `userland/` | init and the injected libraries (window server, SpringBoard, Dynamic Island, GL, media, App Store) |
| `sileo/` | the rootless prefix, Sileo, ElleKit, PreferenceLoader |
| `wifi/` | Linux ath12k and the layer that runs it in XNU |
| `firmware/` | the phone's own GPU, Wi-Fi, vibration and audio DSP firmware |
| `tools/`, `scripts/` | builds, the image assembler, boot and run |
| `.github/workflows/` | the build of the release kit |

## Credits

@netusd · @wannqn · @oodze

XNU-derived code is under the APSL 2.0. Third-party parts keep their licences: Linux ath12k (BSD-3-Clause-Clear
/ GPL-2.0), Mesa (MIT), Mu / Project Silicium (BSD-2-Clause-Patent), zstd (BSD), ElleKit, PreferenceLoader,
Sileo, Procursus; the Noto Sans notice is in `uefi/branding/assets/`. The files in `firmware/` belong to OnePlus
and Qualcomm.

Not affiliated with Apple or OnePlus. iOS is Apple's: this project does not ship it, it downloads it from Apple.
