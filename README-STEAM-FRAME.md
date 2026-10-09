# ASTRO BOT Rescue Mission on the Steam Frame

This is the third way to play AstroQuest, next to the Quest 3 app and the PC: **on Valve's Steam
Frame on its own, as a native Linux program.** The Frame runs SteamOS on an ARM64 Snapdragon chip,
so the emulator does not need an Android app around it (and not Lepton, the Frame's Android
layer): it runs as an ordinary Linux ARM64 program and shows the game through the headset's own
OpenXR runtime.

> **Status: built, not yet played.** The emulator with FEX and the OpenXR host compiles and links
> for aarch64 Linux (cross-compiled with clang 19), and the program starts under qemu as far as
> its command line, but nobody has run it on a Steam Frame yet. Expect to have to look at the
> logs. Please report what you find.

> **No game files are included or distributed.** You need your own copy of ASTRO BOT Rescue
> Mission (CUSA12392, version 1.00 or 1.04), dumped from your own PlayStation 4 - the same as for
> the other two ways, see [README.md](README.md).

## How it fits together

```
Steam Frame (SteamOS, ARM64)
└─ astroquest.sh  (started by Steam, in the Steam Linux Runtime 3.0 ARM64 "sniper")
   └─ shadps4  (aarch64 Linux, glibc)  - the same emulator core as on the Quest
      ├─ FEX runs the game's x86-64 code
      ├─ Vulkan through the system's driver (Mesa Turnip on the Adreno GPU)
      ├─ OpenXR host inside the emulator (the one the PC build uses): head pose, the Frame's
      │  controllers, hand tracking where the runtime offers it, frames to the compositor,
      │  which reprojects them like PSVR did
      ├─ gamepads (DualSense ...) through SDL, as on the PC
      ├─ sound and microphone through SDL (PipeWire)
      └─ emulated PSVR: HMD, tracker, camera, reprojection, social screen
```

What the Quest app does in its own Android process (the OpenXR session, controllers, sound,
microphone) the emulator does itself here, with the code the PC build has had for SteamVR, Virtual
Desktop and the Index. From the Quest build it keeps what suits a headset with a phone's GPU: the
memory budget of a GPU that shares the system's memory, the GPU work sent in parts to a tiling
GPU, the address-space layout FEX needs, and the defaults (field of view 85%, 90 Hz, the emulator
choosing pace and picture size, sharpening) - except multisampling, which stays on (4 samples):
without it the game's edge pass leaves black blocks in the right eye.

## What you need

- A **Steam Frame** in Developer Mode (Settings > System > Developer Mode).
- A PC with the **SteamOS Devkit Client**, paired with the headset (Settings > Developer > Pair
  new host on the headset, then Register in the client's Devkits tab).
- Your **game**: the folder `CUSA12392` (with `eboot.bin`, `sce_sys`, `sce_module`), and an
  update, if you have one, as `CUSA12392-UPDATE` next to it. A `.pkg` has to be unpacked first,
  as described in [README.md](README.md).
- Something to play with: the Frame's own controllers do (the right one is the controller in the
  game, as with the Quest's), or a DualSense paired with the headset over Bluetooth.

## Building

There is no ready-made build yet; it is built from this repository with
[`steam-frame/build.sh`](steam-frame/build.sh), which fetches the libraries and FEX at the
commits the project pins, builds FEXCore, the emulator (`-DENABLE_STEAM_FRAME=ON`) and puts
together `build/AstroQuest-SteamFrame/`.

**For the headset: on an ARM64 Linux machine, in the Steam Linux Runtime's arm64 SDK.** Steam
starts Linux ARM64 programs on the Frame in the Steam Linux Runtime 3.0 (sniper), which has an
older C library than current distributions; a program built against a newer one does not start
there. Build in sniper's SDK for arm64, which Valve publishes as the container image
`registry.gitlab.steamos.cloud/steamrt/sniper/sdk/arm64:latest`. On an aarch64 machine (a
Raspberry Pi 5, an ARM cloud machine, an ARM Mac with a Linux VM) it runs natively; on an
x86-64 PC, Docker runs it through qemu (Docker Desktop does that by itself, also with WSL 2;
elsewhere `docker run --privileged --rm tonistiigi/binfmt --install arm64` sets it up once),
several times slower:

```sh
# in this repository (on Windows: inside WSL, in its own file system, not under /mnt/c)
docker run --rm -it --platform linux/arm64 -v "$PWD":/src -w /src \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk/arm64:latest bash
# then, inside the container:
git config --global --add safe.directory '*'
steam-frame/build.sh
```

The script uses the newest `clang-NN` it finds (17 or later; `STEAM_FRAME_CLANG=clang-19` names
one). What the build needs besides: `git cmake ninja python3`, the development files of
libuuid and udev, and for SDL's backends those of PipeWire/PulseAudio/ALSA and of X11 or Wayland
(set `STEAM_FRAME_SDL_X11=0` to build without the last two: the headset needs no window).
`STEAM_FRAME_CMAKE_ARGS` passes more to the emulator's configuration
(`-DLIBUSB_ENABLE_UDEV=OFF` where there is no udev).

**Cross-compiling on an x86-64 Linux machine** works as well (`clang lld g++-aarch64-linux-gnu
libc6-dev-arm64-cross` on Debian or Ubuntu):

```sh
STEAM_FRAME_CROSS=1 STEAM_FRAME_SDL_X11=0 steam-frame/build.sh
```

That builds against the cross packages' C library, which is newer than sniper's, and without the
sound backends unless their arm64 development files are in the sysroot
(`STEAM_FRAME_SYSROOT=<dir>` for one of your own, for example exported from the sniper arm64
SDK): fine to check that everything compiles, not what to put on the headset.
`build/AstroQuest-SteamFrame/bin/newest-glibc-symbol.txt` says which C library a build needs;
sniper has 2.31.

The individual steps: `steam-frame/build.sh fetch | fex | core | package`.

## Installing

1. **Put the game in the package** (the first upload then takes a while; later ones only send
   what changed): copy `CUSA12392` (and `CUSA12392-UPDATE`, if you have it) into
   `build/AstroQuest-SteamFrame/games/`.

   Or leave it out of the package and copy it to the headset yourself, over the SSH access the
   Devkit Client sets up, to `~/.local/share/AstroQuest/games/CUSA12392` (or `~/AstroQuest/games/`).
2. In the Devkit Client: **Title Upload**
   - Name: `AstroQuest`
   - Local Folder: `build/AstroQuest-SteamFrame`
   - Start Command: `astroquest.sh`
   - Runtime: **Steam Linux Runtime 3.0 ARM64 (Sniper)**
3. **Upload**, then **Start** - or start it on the headset from Library > Non-Steam >
   "Devkit Game: AstroQuest".

If you play with a gamepad, pair it in the headset's Bluetooth settings. For a DualSense to
reach the game with its touchpad and motion sensors, turn **Steam Input off** for the game
(its controller settings in Steam): with Steam Input on, the game gets the virtual controller
Steam makes of it.

## Playing

As on the Quest and the PC (see [README.md](README.md) for all of it):

- **The Frame's controllers**: the right one is the controller in the game. Left stick to move;
  A is ✕, B is □, X is ○, Y is △; the left menu button is OPTIONS. X and Y together blow.
  Right trigger: the touchpad held (water cannon, guns); right grip: a swipe forward (hook,
  stars, chests); left trigger: pull back and let go (the catapult); right stick: a finger on
  the touchpad.
- **A DualSense**: everything as on the console. Where the runtime tracks hands, the gamepad is
  where your hands are; otherwise at a fixed spot in front of you.
- **Reset the view**: hold OPTIONS for a second, press the PS button, or press both sticks of
  the Frame's controllers in.
- **Turn without turning**: hold the left grip (L1 on a gamepad) and flick the right stick.
- Taking the headset off pauses the game.

## Settings

`settings.txt` next to `astroquest.sh` (in the uploaded folder), one `key=value` a line; every
key and its default are explained in the file. The most useful:

| Setting | Meaning |
| --- | --- |
| `fov=85` | how much of the headset's field of view the game draws (50-100): less is sharper, with a soft border |
| `refresh_rate=90` | the display rate asked for, where the runtime lets a program choose (otherwise SteamVR's setting applies): 45/30 fps at 90 Hz, 60/40/30 at 120 |
| `resolution=` | the size of the scene per eye: `816`, `960`, `1200`, `1440`, larger widths, or `game`; empty: the emulator chooses by what the GPU manages |
| `msaa=4` | the most samples a pixel gets. The default, 4, lets the game's multisampling work; `1` is faster but leaves black blocks in steps along edges in the right eye |
| `sharpen=0.6` | sharpening, 0-1 |
| `language=` | the game's language (`de-DE`, `fr-FR` ...); empty: the system's |
| `window=1` | also open a window (for a Linux PC with a headset) |

The same keys can be set without editing the file, in the game's Steam launch options:
`ASTROQUEST_FOV=90 ASTROQUEST_RESOLUTION=1200 %command%`. `./astroquest.sh --check` (from a
shell) shows the game it found and everything it would hand to the emulator.

## Saves and logs

Everything the emulator keeps is in `~/.local/share/AstroQuest/` on the headset (`data=` in the
settings names another folder), outside the uploaded folder, so a new upload leaves it alone:

- `user/home/1000/savedata/CUSA12392` - your saves;
- `user/log/shad_log.txt` - the emulator's log: the OpenXR runtime and headset it found
  (`Headset found through ...`), the frame rate and where its time goes every few seconds, sound,
  microphone, controllers. `console.txt` is what it printed. The `.prev` files are the run
  before.

What to look for when it does not work:

| In the log | Meaning |
| --- | --- |
| `No OpenXR runtime that could show frames in a headset` | the OpenXR loader found no runtime in the Steam Linux Runtime's container: the game is shown nowhere. Check `XR_RUNTIME_JSON` (`astroquest.sh --check` prints it) |
| `has no headset connected yet` | the runtime answered but had no headset; it is picked up when there is one |
| `Mapping cannot fit inside free region` | the console's address space did not fit next to what is loaded in the process |
| `vkCreateInstance` / `No suitable GPU` errors | the Vulkan driver was not reachable in the container |

## What was changed

For the Frame build (everything else is the emulator as it is):

- `CMakeLists.txt`: the option `ENABLE_STEAM_FRAME` (which also links the C++ library into the
  program, so that it does not depend on the runtime's); `ENABLE_OPENXR` is now available on
  Linux as well as Windows; `SHADPS4_MOBILE_GPU` stands for what the Quest build and the Frame
  build share (memory budget, staging buffer size, cache thresholds, VMA block size, draws sent
  to the GPU in parts).
- `src/core/vr/openxr_host.cpp`: builds on Linux (the Windows sound-device lookup is Windows
  only; Linux runtimes get the file-descriptor and dma-buf Vulkan extensions); the headset's
  sound is the system's default, or `SHADPS4_XR_AUDIO_OUTPUT` / `SHADPS4_XR_AUDIO_INPUT`;
  `SHADPS4_XR_REFRESH_RATE` asks for a display rate through `XR_FB_display_refresh_rate` where
  the runtime has it.
- `src/core/address_space.cpp`: the Frame lays the console's address space out the way the
  Quest build does (at the console's own addresses, below and around whatever the process has
  mapped already: FEX does not offset addresses, and SteamVR's libraries are in the process).
- `src/video_core/...`: the mobile-GPU settings above, the pipeline cache warm-up skipped as
  under FEX on the Quest, and in `vk_platform.cpp` the OpenXR header included after the
  emulator's own (on Linux the Vulkan headers bring Xlib, whose `None` macro clashed).
- `externals/CMakeLists.txt`, `externals/openxr-sdk/src/cmake/presentation.cmake`: the OpenXR
  loader with its own jsoncpp, and without X11 when only the loader is built.
- `steam-frame/`: `build.sh`, `toolchain-aarch64-cross.cmake`, `astroquest.sh`, `settings.txt`.

The Quest app (`quest-host/`) is not used: it is an Android app (Java, OpenGL ES,
AHardwareBuffer, an Android sandbox for the emulator), none of which there is on SteamOS.

## Open questions

Things only the first run on a headset will tell:

- Whether the OpenXR runtime is reachable from the Steam Linux Runtime container as Steam
  starts the program (Valve recommends OpenXR for native Linux ARM64 programs on the Frame, so
  it should be).
- Which interaction profile the Frame's controllers answer to: by default they present
  themselves as Touch controllers, which the emulator binds; the Frame's own profile is not
  bound yet.
- Whether the runtime offers hand tracking (`XR_EXT_hand_tracking`), which places a gamepad;
  without it a gamepad stays at a fixed spot in front of you.
- Whether the runtime lets a program choose the display rate; otherwise set 90 Hz (or 120) in
  SteamVR's settings yourself.
- How fast it is: the Frame's Snapdragon 8 Gen 3 has a somewhat faster GPU than the Quest 3's,
  and twice its memory; the emulator chooses pace and picture size by what it measures.
