<p align="center">
  <img src="uefi/branding/assets/logo.png" width="120" alt="HackIOS">
</p>

<h1 align="center">ios-macan</h1>

<p align="center"><strong>Real iOS 16.5 on the OnePlus Ace 6T.</strong><br>
Apple's own XNU kernel and iOS userland, booted straight from RAM.<br>
No emulator · nothing flashed · one reboot and Android is back</p>

---

## What is this

`ios-macan` starts Apple's XNU kernel and the iOS 16.5 userland (the iPhone X build) on the **OnePlus Ace 6T**
(PLR110, Snapdragon SM8845, Adreno 829). Not a skin, not a launcher: SpringBoard, Settings, apps, Sileo and
tweaks run on the phone's own CPU and GPU.

Nothing is written to the phone. Everything is sent over USB into RAM; reboot the phone and Android is exactly as
it was. Apple's files are not in this repository: the script downloads them from Apple's servers on your PC.

## What works

| | Status | Notes |
| :--- | :---: | :--- |
| **CPU, interrupts, timers** | ✅ | all 8 cores, six at full clock |
| **Display** | ✅ | iPhone X layout (375 × 812 points) |
| **GPU** | ✅ | OpenGL ES through Mesa on the Adreno 829; the whole interface is GPU-drawn, 80–100 fps |
| **Metal** | ❌ | apps that need Metal do not run |
| **Touch** | ✅ | multi-touch |
| **Buttons** | ✅ | power, volume up, volume down |
| **Wi-Fi** | ✅ | joins your network, DHCP, Internet |
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

## Before you start

You need:

1. **A OnePlus Ace 6T with an unlocked bootloader** (`fastboot boot` must work).
2. **Root in Android** (Magisk, KernelSU or APatch) and **USB debugging** on: the script uses them to reboot
   the phone into the bootloader and to copy the phone's own firmware (GPU, Wi-Fi, vibration, audio).
3. **A USB cable**, a PC with **Windows 10/11 or Linux**, and about **15 GB** of free disk space.

What gets downloaded, and from where:

| | From |
| :--- | :--- |
| the kernel, the UEFI image, our userland (`hackios-kit.zip`) | this repository's [Releases](../../releases), built by GitHub Actions from this code |
| iOS 16.5 (about 7 GB) | Apple's own servers, on your PC: it is never redistributed |
| GPU, Wi-Fi, vibration and audio firmware | your phone, over USB, from its Android |

## Tutorial: Windows 10 / 11

1. **Install Python 3** (once). Open *Terminal* or *Command Prompt* and run:
   ```bat
   winget install -e --id Python.Python.3.12
   ```
   (or get it from [python.org](https://www.python.org/downloads/) and tick *Add python.exe to PATH*).
2. **Download this repository**: the green *Code* button → *Download ZIP*, unpack it, and open a terminal in
   the unpacked folder (or `git clone https://github.com/hxfuxyy/ios-macan.git` and `cd ios-macan`).
3. **Plug in the phone**, booted into Android, unlocked, rooted, USB debugging on. Accept the
   *Allow USB debugging* prompt on the phone and grant root to *Shell* when Magisk/KernelSU asks.
4. **Tell it your Wi-Fi** (optional, but without it iOS has no Internet):
   ```bat
   hackios.bat --wifi "Your Wi-Fi name" "your-password"
   ```
5. **Get everything** (the first time takes a while: it downloads about 7 GB from Apple):
   ```bat
   hackios.bat --get
   ```
   It downloads the release kit, iOS 16.5 from Apple, copies the firmware from the phone and makes the images.
   `adb`, `fastboot` and 7-Zip are installed automatically if they are missing.
6. **Start iOS**:
   ```bat
   hackios.bat --boot
   ```
   The phone reboots into the bootloader, then into iOS. Nothing is flashed.
7. **Back to Android**: `hackios.bat --android`.

Next time only step 6 is needed.

## Tutorial: Linux

```sh
git clone https://github.com/hxfuxyy/ios-macan.git
cd ios-macan
./hackios --wifi "Your Wi-Fi name" "your-password"
./hackios --get
./hackios --boot
```

Needed: python3, 7z (`p7zip-full` or `7zip`), adb and fastboot, and a C compiler (for one small tool). On
Ubuntu/Debian: `sudo apt install python3 p7zip-full adb fastboot gcc`.

## Building everything yourself

The release kit is built by [GitHub Actions](.github/workflows/build.yml) from this code; you can build the same
on Linux (on Windows inside WSL 2):

```sh
./hackios --install     # toolchain, Apple's XNU + kernel/, ath12k, Mesa, iOS from Apple, firmware, then --build
./hackios --build       # userland, Sileo root, kernel, UEFI (from uefi/mu), images
./hackios --boot
```

Needed: clang, lld, llvm, make, flex, bison, perl, python3, meson, ninja, git, curl, 7z, zstd, adb, fastboot.
The theos toolchain and the iOS 16.5 SDK are downloaded into `work/theos` automatically. The UEFI firmware
source is complete in `uefi/mu` (no external checkout).

## All commands

On Windows use `hackios.bat`, on Linux `./hackios`.

| Command | What it does |
| :--- | :--- |
| `--get` | the release kit, iOS from Apple, the phone's firmware, the images: everything to boot |
| `--boot` | Android, fastboot or the loader → iOS, all from RAM |
| `--boot -- OPTIONS` | the same with options (below) |
| `--android` | back to Android |
| `--wifi NAME PASS` | the Wi-Fi network iOS joins (then `--assemble`, or `--get` if not done yet) |
| `--status` | what is connected and what is ready |
| `--fetch`, `--firmware`, `--assemble` | single steps of `--get` |
| `--run` | iOS on a phone already in the loader |
| `--install`, `--deps`, `--build`, `--kernel`, `--clean` | building everything yourself (Linux / WSL) |
| `--tools` | (Windows) download adb and fastboot |

Options after `--`:

| Option | |
| :--- | :--- |
| `--seconds N` | come back to the PC after N seconds with the kernel's log (`0`: run until you reboot) |
| `--island 2` | turn on the Dynamic Island |
| `--no-wifi`, `--no-gpu` | without Wi-Fi, or with software drawing |
| `--haptic 0`, `--torch 0` | without vibration or flashlight |
| `--cpus N` | use fewer CPU cores |

Example: `hackios.bat --boot -- --island 2 --seconds 300`

## If something goes wrong

- **"no phone over USB"**: check the cable, that USB debugging is on, and that the phone asked to allow the PC.
- **"Android has no working root"**: grant root to *Shell* in Magisk/KernelSU (`adb shell su -c id` must say `uid=0`).
- **Windows does not see the phone in fastboot or in the loader**: install the
  [Google USB driver](https://developer.android.com/studio/run/win-usb), or bind *WinUSB* to the device with
  [Zadig](https://zadig.akeo.ie/). The iOS loader shows up as a fastboot device named `xnuloader`.
- **Stuck on the Apple logo or a black screen**: reboot into Android and run `--boot` again.
- **No Wi-Fi in iOS**: run `--wifi` with your network, then `--assemble`, then `--boot`.
- **Windows support is new**: building the iOS images natively on Windows has been checked less than on Linux.
  If `--get` fails on Windows, run the same steps inside WSL (`wsl ./hackios --get`) and boot from Windows.

## How it boots

```text
Android ─▶ bootloader ── fastboot boot (RAM) ──▶ UEFI (uefi/) ──▶ USB loader (loader/)
        ──▶ XNU 8796.121.2 + kernel/ ──▶ root image + iOS system image in RAM ──▶ launchd ──▶ SpringBoard
```

## Inside

| | |
| :--- | :--- |
| `hackios`, `hackios.bat` | the one entry point (Linux, Windows) |
| `.github/workflows/` | the build of the release kit |
| `kernel/` | `kernel.patch` (Apple's files) and `files/` (the phone's drivers `macan_*`, GPU, packfs, IOKit services) |
| `loader/` | the USB loader that takes the kernel and the images |
| `uefi/` | the complete UEFI firmware source (`uefi/mu`: Project Mu with the Ace 6T platform, menu and branding) |
| `userland/` | init, injected libraries (window server, SpringBoard, Dynamic Island, GL, media, App Store), images |
| `sileo/` | the rootless prefix, Sileo, ElleKit, PreferenceLoader |
| `wifi/` | Linux ath12k and the layer that runs it in XNU |
| `tools/` | kernel build, Mesa and its Darwin patch, ath12k build, packfs, the image assembler, firmware staging, `xl` |
| `scripts/` | boot and run |

## Credits

OOSexGang: [@netusd](https://t.me/netusd), [@wannqn](https://t.me/wannqn), [@hxfuxyyXD](https://t.me/hxfuxyyXD),
[@oodze](https://t.me/oodze) · [channel](https://t.me/oosexgangdevs). Started from vcvkk's POCO X5 Pro port.

XNU-derived code is under the APSL 2.0. Third-party parts keep their licences: Linux ath12k (BSD-3-Clause-Clear
/ GPL-2.0), Mesa (MIT), Mu / Project Silicium (BSD-2-Clause-Patent), zstd (BSD), ElleKit, PreferenceLoader,
Sileo, Procursus; the Noto Sans notice is in `uefi/branding/assets/`.

Not affiliated with Apple or OnePlus. iOS is Apple's: this project does not ship it, it downloads it from Apple
for you.
