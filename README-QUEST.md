# Wipeout Pure VR (Meta Quest + PICO)

A standalone VR port of **Wipeout Pure** (PSP, Studio Liverpool, 2005) for Meta Quest and PICO headsets, built on
[pure-recomp](https://github.com/erreib/pure-recomp), a native port of the game made by static recompilation (no
emulator). Races run in stereo 3D around you with head tracking; the menus, movies and the port's settings are on a
curved screen.

- Races at 60 fps and full game speed on Meta Quest 3, shown at 120 Hz (every frame exactly twice, no judder)
- Stereo 3D with head tracking: look into the corners and over the edge of the track; the HUD floats in front of you
- Antialiasing: supersampled eyes plus 2x MSAA
- The HD texture pack by NR74W, downloaded by the app on the first start (nothing of it is in the APK)
- Touch and PICO controllers, the menus also on the left stick
- Shader pipelines for the whole game prepared in the background, so races do not stop to compile shaders

**You need a disc image dumped from your own copy of Wipeout Pure, US Greatest Hits edition (UCUS-98612, version
2.00).** The app checks it and refuses other editions and regions. The APK contains none of the game.

## Headsets

| Headset | Status |
|---|---|
| Meta Quest 3 / 3S | tested |
| Meta Quest 2 / Pro | untested; gets lighter defaults (smaller eye images, no MSAA) |
| PICO 4 Ultra | untested |
| PICO 4 / Neo 3 | untested; lighter defaults as on Quest 2 |

## Install

1. Install the APK: with [SideQuest](https://sidequestvr.com/) (drag the APK onto it) or
   `adb install -r WipeoutPure-VR-<version>.apk`. On PICO, turn on developer mode and USB debugging first.
2. Start **Wipeout Pure VR** once (Quest: Library > Unknown Sources). It creates its folder and shows the setup
   screen, which can't be used in VR: quit with the Meta / PICO button.
3. Copy your disc image (an `.iso`, any name) into the app's `import` folder:

   `Android/data/online.portroyale.wipeoutpurevr/files/import/`

   With a PC: connect the headset by USB, allow file access in the headset, then open it in your file manager
   (Windows: This PC > Quest 3 > Internal shared storage > Android > data > ...). With SideQuest: the folder icon
   ("Manage files on the headset"). With adb:
   `adb push WipeoutPure.iso /sdcard/Android/data/online.portroyale.wipeoutpurevr/files/import/`
4. Start the app again. It checks the image, moves it into `files/game/` and prepares the game from it (a few
   seconds); the game starts. If the image is not the supported one, `import/SETUP-ERROR.txt` says why.
5. **HD textures:** with Wi-Fi, the app downloads the HD texture pack (about 450 MB) from its author's GitHub
   repository in the background while you play. When it is done the textures switch to HD by themselves, no
   restart needed. `files/hd-textures.txt` shows how far it is; if the download fails it is tried again at the next
   start.
   - Don't want them: create an empty file named `no-hd-textures` in `files/` before the first start.
   - Have the pack already: copy its folder (the one holding `textures.ini`) to `files/textures/UCUS98612/`.

Your saves and settings are in `files/` too (saves under `files/game/ms0/PSP/SAVEDATA/`, in the PSP's own
format, so saves from a PSP or PPSSPP can be copied there). **Uninstalling the app deletes this folder:** copy it to
your PC first.

## Controls

| Controller | In a race | In the menus |
|---|---|---|
| Left stick | steer | move the cursor |
| Right trigger or A | thrust | confirm |
| Left trigger | fire weapon | |
| Left / right grip | left / right airbrake | |
| X | absorb the weapon (shield energy) | |
| B | look behind | back |
| Y | change the camera view | |
| Menu button or left stick click | pause | |
| Left stick up / down | (pause menu, while not thrusting) | |
| Right stick | D-pad | D-pad |
| Both stick clicks | the port's settings menu | |

Hold the Meta / PICO button to re-centre the view.

## Settings for testers

A text file `files/env.txt` with `KEY=VALUE` lines is read at every start, so the VR settings can be changed
without a new build:

| Line | Effect |
|---|---|
| `PURE_VR_EYE_SCALE=0.9` | eye image size, relative to the headset's recommendation (0.7 on XR2 Gen 1 headsets) |
| `PURE_VR_MSAA=2` | antialiasing samples: 0 (off), 2 or 4 (0 on XR2 Gen 1 headsets) |
| `PURE_VR_REFRESH=120` | display refresh in Hz, if the headset offers it (72 = the system default) |
| `PURE_VR_RACE_FPS=30` | races at the PSP's 30 fps, for headsets that can't hold 60 |
| `PURE_VR_HUD_DISTANCE=2.0`, `PURE_VR_HUD_WIDTH=1.8` | where the HUD floats, in metres |
| `PURE_VR_FOV=110` | how wide the game draws the track around you, in degrees |
| `PURE_PERF=log` | a performance line every second in `files/PureNative.log` |

The log of the last run is `files/PureNative.log` (and of the one before, `PureNative-previous.log`): please
attach both to bug reports.

## Known issues

- The first races on a new install may hitch briefly when the game shows an effect for the first time; it is
  remembered, so it happens once.
- Without an imported disc image the app shows the desktop setup screen, which can't be used in VR (step 3 above).
- No online (ad hoc) multiplayer yet.
- PICO and the XR2 Gen 1 headsets (Quest 2/Pro, PICO 4) are untested.

## Building

The game's code is generated from the disc image, so builds are made on a machine that has it: generate the corpus
as the main [README](README.md) describes (Linux: see `docs/` and `tools/regen.sh`), then

```
ANDROID_HOME=<sdk> ANDROID_NDK=<ndk 27> PURE_QUEST_DEPS=<deps dir> \
  RELEASE=1 VERSION_NAME=<version> packaging/quest/build_quest.sh
```

`packaging/quest/build_quest.sh` fetches and builds SDL3 and the Khronos OpenXR loader, compiles the game and packs
the APK without Gradle. The VR code is `src/vr/quest_xr.cpp` (OpenXR) and the renderer bridge in `tools/quest/`
(`xr_bridge.hpp`, `stereo_replay.inc`, applied to PortableKit as `patches/0035-openxr-bridge.patch` by
`tools/quest/xr_bridge_edits.py`).

## Credits

- [pure-recomp](https://github.com/erreib/pure-recomp) by erreib: the port this is built on (GPL-2.0)
- [PortableKit](https://github.com/TeamGDB/PortableKit), [Yakumo](https://github.com/TeamGDB/Yakumo) and
  [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp): the framework and its lineage
- The WipEout Pure HD texture pack by NR74W, [re-uploaded by 0xf0xx0](https://github.com/0xf0xx0/WipEout-Pure-HD-Texture-Pack)
  (downloaded from there by the app)
- [SDL3](https://libsdl.org/), the [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK),
  [FFmpeg](https://ffmpeg.org/) (LGPL) and the [DejaVu fonts](https://dejavu-fonts.github.io/)
- VR controls modelled on [Wipeout-Rewrite-VR](https://github.com/SgtBilko76/Wipeout-Rewrite-VR)

Wipeout Pure was developed by Studio Liverpool and published by Sony Computer Entertainment Europe. WIPEOUT is a
registered trademark of Sony Interactive Entertainment. This project is not endorsed, sponsored or authorized by
Sony Interactive Entertainment.
