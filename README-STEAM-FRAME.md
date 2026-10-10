# ASTRO BOT Rescue Mission on the Steam Frame

This is the third way to play AstroQuest, next to the Quest 3 app and the PC: **on Valve's Steam
Frame on its own, as a native Linux program.** The Frame runs SteamOS on an ARM64 Snapdragon chip,
so the emulator does not need an Android app around it (and not Lepton, the Frame's Android
layer): it runs as an ordinary Linux ARM64 program and shows the game through the headset's own
OpenXR runtime.

> **Status: plays on a Steam Frame.** Built in the Steam Linux Runtime's arm64 SDK (also on an
> x86 PC, in that SDK's container under emulation), uploaded and started with the Devkit
> Client, shown through SteamVR's OpenXR runtime. The game runs at its full speed at 36 frames a
> second (the display at 72 Hz, two refreshes a frame); with foveated rendering and eye tracking
> the scene stays at 1440x1536 an eye even in the busiest places of the first worlds. Still
> early: please report what you find.

> **No game files are included or distributed.** You need your own copy of ASTRO BOT Rescue
> Mission (CUSA12392, version 1.00 or 1.04), dumped from your own PlayStation 4 - the same as for
> the other two ways, see [README.md](README.md).

## How it fits together

```
Steam Frame (SteamOS, ARM64)
└─ astroquest.sh  (started by Steam, in the Steam Linux Runtime 3.0 ARM64 "sniper")
   └─ shadps4  (aarch64 Linux, glibc)  - the same emulator core as on the Quest
      ├─ FEX runs the game's x86-64 code
      ├─ Vulkan through the system's driver (Mesa Turnip on the Adreno 750)
      │  └─ foveated rendering: the scene shaded more coarsely away from where you look
      ├─ OpenXR host inside the emulator (the one the PC build uses): head pose, the Frame's
      │  controllers, hand tracking, eye tracking, frames to the compositor, which reprojects
      │  them like PSVR did
      ├─ gamepads (DualSense ...) through SDL, as on the PC
      ├─ sound and microphone through SDL (PipeWire)
      └─ emulated PSVR: HMD, tracker, camera, reprojection, social screen
```

What the Quest app does in its own Android process (the OpenXR session, controllers, sound,
microphone) the emulator does itself here, with the code the PC build has had for SteamVR, Virtual
Desktop and the Index. From the Quest build it keeps what suits a headset with a phone's GPU: the
memory budget of a GPU that shares the system's memory, the GPU work sent in parts to a tiling
GPU, the address-space layout FEX needs, and the defaults (field of view 85%, no multisampling
with the driver drawing straight into memory, the emulator choosing pace and picture size,
sharpening). On top of that the Frame build has what the Quest's runtime does not give an
emulator in another process: the headset's eye tracking, for foveated rendering.

## What you need

- A **Steam Frame** in Developer Mode (Settings > System > Developer Mode).
- A PC with the **SteamOS Devkit Client** (`steam://install/943760` in Steam), paired with the
  headset (Settings > Developer > Pair new host on the headset, then Register in the client's
  Devkits tab).
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

### In the Steam Linux Runtime's arm64 SDK (for the headset)

Steam starts Linux ARM64 programs on the Frame in the Steam Linux Runtime 3.0 (sniper), whose C
library (glibc 2.31) is older than current distributions'; a program built against a newer one
does not start there. So the build for the headset is made in sniper's SDK for arm64, which Valve
publishes as the container image `registry.gitlab.steamos.cloud/steamrt/sniper/sdk/arm64:latest`.
On an aarch64 machine (a Raspberry Pi 5, an ARM cloud machine, an ARM Mac with a Linux VM) it
runs natively; on an x86-64 PC, Docker runs it through qemu (Docker Desktop does that by itself,
also with WSL 2; elsewhere `docker run --privileged --rm tonistiigi/binfmt --install arm64` sets
it up once). Under emulation the first build takes hours, later ones only rebuild what changed.

**1. Make a container you keep** (on Windows: in WSL, with the repository in WSL's own file
system, for example `~/AstroQuest`, not under `/mnt/c`):

```sh
docker run -it --name astroquest-build --platform linux/arm64 \
    -v ~/AstroQuest:/src/AstroQuest -w /src/AstroQuest \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk/arm64:latest bash
# later, to come back to it:
docker start -ai astroquest-build
```

**2. Add a newer clang and CMake, once.** The SDK has clang 11, the build needs 17 or later
(FEX); and openal-soft needs CMake 3.26 or later. Inside the container:

```sh
git config --global --add safe.directory '*'
# clang 19 from apt.llvm.org (sniper is Debian 11 "bullseye" underneath)
wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key
echo "deb http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-19 main" \
    > /etc/apt/sources.list.d/llvm.list
apt-get update && apt-get install -y clang-19 lld-19
# CMake from Kitware's release
mkdir -p /opt && cd /opt
wget https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-aarch64.tar.gz
tar xf cmake-3.31.6-linux-aarch64.tar.gz
ln -sf /opt/cmake-3.31.6-linux-aarch64/bin/* /usr/local/bin/
cd /src/AstroQuest
```

(If `apt-get update` fails on one of the SDK's own sources, comment that line out in
`/etc/apt/sources.list` and try again.)

**3. Build:**

```sh
STEAM_FRAME_SDL_X11=0 steam-frame/build.sh
```

The script picks the newest `clang-NN` it finds (`STEAM_FRAME_CLANG=clang-19` names one) and
takes the C++ library from the newest GCC in the SDK, linked into the program, so that it does
not depend on the runtime's. `STEAM_FRAME_SDL_X11=0` builds SDL without X11 and Wayland: the
headset needs no window. `STEAM_FRAME_CMAKE_ARGS` passes more to the emulator's configuration.
At the end, `build/AstroQuest-SteamFrame/bin/needed-libraries.txt` lists the libraries the
program needs from the system and `newest-glibc-symbol.txt` the newest C library function it
uses (nothing newer than 2.31 may be in it).

**After a `git pull`**, only the emulator has to be built again (FEX only changes when the
project pins another version of it):

```sh
steam-frame/build.sh core && steam-frame/build.sh package
```

The individual steps are `steam-frame/build.sh fetch | fex | core | package`.

### Cross-compiling (to check that it compiles)

On an x86-64 Linux machine with `clang lld g++-aarch64-linux-gnu libc6-dev-arm64-cross` (Debian,
Ubuntu):

```sh
STEAM_FRAME_CROSS=1 STEAM_FRAME_SDL_X11=0 steam-frame/build.sh
```

That builds against the cross packages' C library, which is newer than sniper's, and without
the sound backends unless their arm64 development files are in the sysroot
(`STEAM_FRAME_SYSROOT=<dir>`): fine to check that everything compiles, not what to put on the
headset.

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

An upload replaces `settings.txt` in the uploaded folder with the one from the package: settings
you want to keep are best given in the game's Steam launch options (see [Settings](#settings)).

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
- **A DualSense**: everything as on the console. The Frame tracks hands, so the gamepad is
  where your hands are.
- **Reset the view**: hold OPTIONS for a second, press the PS button, or press both sticks of
  the Frame's controllers in.
- **Turn without turning**: hold the left grip (L1 on a gamepad) and flick the right stick.
- Taking the headset off pauses the game.

## Settings

`settings.txt` next to `astroquest.sh` (in the uploaded folder), one `key=value` a line; every
key and its default are explained in the file. The most useful:

| Setting | Meaning |
| --- | --- |
| `foveation=0` | foveated rendering: `1`-`3` shade the scene ever more coarsely away from where you look, so the GPU has time for a larger picture. `2` is a good start: the scene then stays at full size where it otherwise drops to 960x1080 |
| `eye_tracking=1` | with foveation on, the sharp middle follows your eyes (the Frame's eye tracking, through SteamVR); `0` keeps it in the middle of the picture |
| `overlay=0` | `1`: a debug overlay in the headset (frame rate, frame time, display rate, scene size, GPU load, draws, foveation, gaze) |
| `foveation_colors=0` | `1`: foveation's rings in colour over the picture (green: every pixel, yellow: 2x2, red: 4x4) and a mark where you look |
| `resolution=` | the size of the scene per eye: `816`, `960`, `1200`, `1440`, larger widths, or `game`; empty: the emulator chooses, as large as the GPU manages (at most 1440x1536) |
| `msaa=1` | the most samples a pixel gets: 1 (the default) is fastest, the emulator smooths edges itself; `4` lets the game multisample as on the console, for more GPU time |
| `fov=85` | how much of the headset's field of view the game draws (50-100): less is sharper, with a soft border |
| `sharpen=0.6` | sharpening, 0-1 |
| `refresh_rate=90` | the display rate asked for, where the runtime lets a program choose. SteamVR on the Frame offers only 72 Hz to programs at the moment, which gives 36 frames a second |
| `language=` | the game's language (`de-DE`, `fr-FR` ...); empty: the system's |
| `window=1` | also open a window (for a Linux PC with a headset) |

The same keys can be set without editing the file, in the game's Steam launch options, where
they also survive a new upload: `ASTROQUEST_FOVEATION=2 ASTROQUEST_RESOLUTION=1200 %command%`.
`./astroquest.sh --check` (from a shell) shows the game it found and everything it would hand to
the emulator.

### Foveated rendering

With `foveation` on, the passes that draw the scene get a small map of how finely to shade:
every pixel in the middle, one colour for every 2x2 or 4x4 pixels further out. Depth and the
edges of things stay exact at every pixel, only the colour inside them is computed for fewer of
them; the bloom, the picture handed to the headset and the shadow maps are not touched. With
eye tracking the middle is where you look, moved once a frame; when the headset loses your eyes
(a blink) for half a second, it goes back to the middle of the picture.

| Level | Every pixel | 2x2 | 4x4 |
| --- | --- | --- | --- |
| `1` | out to half way to the edge | beyond | - |
| `2` | out to 45% | out to 75% | beyond |
| `3` | out to 35% | out to 60% | beyond |

At 36 frames a second the sharp middle follows a quick glance a frame late: if that shows, a
milder level hides it.

The headset's readings of where you look tremble a little, as eyes do: they are smoothed while
your gaze rests and followed at once when it jumps (`gaze_smoothing=1`, in Hz: smaller is
steadier, `0` takes them as they come). Where the sharp middle sits a little beside where you
look, `gaze_offset=<right>,<up>` moves it by that many degrees. (Each eye sees what you look at
a little to one side of where the head's gaze points - more the closer it is - and the map is
one for both eyes, so up close a degree or two of that is to be expected.)

`foveation_colors=1` shows the rings over the picture: green where every pixel is shaded,
yellow where one colour does for 2x2 pixels, red for 4x4, and a small white ring where the
headset sees you look.

### Debug overlay

`overlay=1` puts a few lines of text above the middle of the picture, at the same place in
both eyes (so they look far away), updated twice a second:

```
36 fps  27.8 ms  72 Hz/2          frames a second, what a frame takes, display rate / refreshes a frame
Scene 1440x1536  GPU 71%          the size the scene is drawn at, how busy the GPU was
Eye 1440x1536  Draws 1874  MSAA 1 the eyes' pictures, draws in the last frame, multisampling
Foveation 2  eyes 7R 3U           the foveation level, where you look (degrees right/left, up/down)
```

`SHADPS4_VR_OVERLAY_SCALE=1..4` makes the text smaller or larger (2 unless told). Both overlays
are drawn over the frame that goes to the headset, after the emulator's own passes, and cost
next to nothing.

## Saves and logs

Everything the emulator keeps is in `~/.local/share/AstroQuest/` on the headset (`data=` in the
settings names another folder), outside the uploaded folder, so a new upload leaves it alone:

- `user/home/1000/savedata/CUSA12392` - your saves;
- `user/log/shad_log.txt` - the emulator's log: the OpenXR runtime and headset it found
  (`Headset found through ...`), what the runtime offers, the frame rate and where its time goes
  every few seconds, the size the scene is drawn at and how busy the GPU is, sound, microphone,
  controllers. `console.txt` is what it printed. The `.prev` files are the run before.

What to look for:

| In the log | Meaning |
| --- | --- |
| `No OpenXR runtime that could show frames in a headset` | the OpenXR loader found no runtime in the Steam Linux Runtime's container: the game is shown nowhere. Check `XR_RUNTIME_JSON` (`astroquest.sh --check` prints it) |
| `has no headset connected yet` | the runtime answered but had no headset; it is picked up when there is one |
| `Mapping cannot fit inside free region` | the console's address space did not fit next to what is loaded in the process |
| `vkCreateInstance` / `No suitable GPU` errors | the Vulkan driver was not reachable in the container |
| `The scene is drawn at ... instead of ...` | the emulator changed the scene's size, and why (how long frames took, how busy the GPU was) |
| `Foveation: passes of ... shade every pixel in ...%` | foveated rendering is on, and how much of the picture is shaded how finely |
| `Foveation asked for, but ...` | the GPU or driver cannot do it: it stays off |
| `The headset can tell where the eyes look` / `tells where the eyes look: now ...` | eye tracking is offered / its first reading arrived; without the second line the middle stays put |

### Looking into the picture

For finding what the GPU draws wrong, two aids that do nothing until asked:

- `touch ~/.local/share/AstroQuest/user/dump_render_targets`: a picture of every render target
  the game has drawn so far, in `user/rt_dump/`, and the next 3000 draws in the log.
- `touch ~/.local/share/AstroQuest/user/dump_draws`: for the next frame, a picture of the render
  target (and of the depth, where a draw writes it) after every single draw, fast clear and
  resolve, in `user/draw_dump/`, named by their order, target, shaders and depth state, with a
  `DRAWDUMP` line in the log for each. The game stands still meanwhile (a few seconds to a
  minute). `SHADPS4_DBG_DRAW_DUMP_CB=<addresses>`, `_MIN_WIDTH`, `_WIDTH`, `_DEPTH=0`,
  `_FRAMES`, `_MAX` narrow it down.

Both are looked for every 120 frames. `SHADPS4_DBG_SYNC=1` puts a full memory barrier after
every draw (slow): a picture that is right with it and wrong without is missing a barrier
somewhere. `SHADPS4_DBG_SKIP_PS=<shader hashes>` leaves out the draws of those pixel shaders.

## What was changed

The Quest and PC builds are not changed by any of this: what is different on the Frame is
either compiled only into its build (`ENABLE_STEAM_FRAME`), or only switched on by a setting its
launcher sets (foveation, eye tracking, the display rate), and the debugging aids do nothing
until asked. Where the Frame shares something with the Quest build, the condition for the
Quest is the same as before.

**Building**

- `CMakeLists.txt`: the option `ENABLE_STEAM_FRAME`, which also links the C++ library into the
  program; `ENABLE_OPENXR` can now be built on Linux (on by default for Windows, as before, and
  for the Frame); `SHADPS4_MOBILE_GPU` stands for what the Quest build and the Frame build share
  (memory budget, staging buffer size, cache thresholds, VMA block size, draws sent to the GPU
  in parts).
- `externals/CMakeLists.txt`, `externals/openxr-sdk/src/cmake/presentation.cmake`: the OpenXR
  loader with its own jsoncpp on the Frame, and without X11 when only the loader is built and
  there is no X11.
- Older C library (glibc 2.31): `epoll_pwait2` through the system call where the C library does
  not name it (`net.cpp`), `SYS_faccessat2` named where the headers do not (`main.cpp`), and a
  missing `MREMAP_DONTUNMAP` for FEX (`steam-frame/fex-old-glibc.patch`).
- `steam-frame/`: `build.sh`, `toolchain-aarch64-cross.cmake`, `fex-old-glibc.patch`,
  `astroquest.sh` (the launcher), `settings.txt`.

**Running**

- `src/core/vr/openxr_host.cpp`: builds on Linux (the Windows sound-device lookup is Windows
  only; Linux runtimes get the file-descriptor and dma-buf Vulkan extensions); the headset's
  sound is the system's default, or `SHADPS4_XR_AUDIO_OUTPUT` / `SHADPS4_XR_AUDIO_INPUT`;
  `SHADPS4_XR_REFRESH_RATE` asks for a display rate through `XR_FB_display_refresh_rate`; the
  extensions the runtime offers are logged; with foveation, the gaze of
  `XR_EXT_eye_gaze_interaction` goes to the renderer (`src/core/vr/eye_gaze.h`), next to the
  title's field of view (`vr_runtime.cpp`).
- `src/core/address_space.cpp`: the Frame lays the console's address space out the way the
  Quest build does (at the console's own addresses, below and around whatever the process has
  mapped already: FEX does not offset addresses, and SteamVR's libraries are in the process).
- `vk_presenter.cpp`: without a window (as on the Frame) the window's look at the eyes' pictures
  is no longer drawn and presented every frame for nobody (`SHADPS4_VR_WINDOW_FRAME=1` keeps
  it); the picture dumps.

**Drawing**

- `vk_scheduler.cpp`: a barrier before every render pass, for what the passes before it drew
  (`SHADPS4_PASS_BARRIER`, on in the Frame build). The texture cache leaves one out where an
  image stays a target from one pass to the next, and the order of pixels only holds within a
  pass; with the driver drawing straight into memory (`TU_DEBUG=sysmem`, used without
  multisampling) passes overlapped, and the right eye's picture had black squares in a grid
  along the edges of things. (Multisampling hid that only because the driver then draws in
  tiles, a pass at a time.)
- `vk_instance.cpp`, `vk_graphics_pipeline.cpp`, `vk_rasterizer.cpp`, `vk_scheduler.cpp`:
  foveated rendering (`SHADPS4_FOVEATION=1..3`) through `VK_KHR_fragment_shading_rate` with a
  shading rate image: the passes that draw the scene (a colour and a depth target of 512 pixels
  or more, not wider than an eye's picture) get a map of 8x8-pixel texels, moved with the gaze
  (`SHADPS4_FOVEATION_EYES=0`: it stays put; `_INNER`, `_OUTER` move the rings). Pixel shaders
  that write depth keep to every pixel where the GPU cannot shade those coarser (the Frame's
  can).
- `vk_pipeline_cache.cpp`: the barriers in compute shaders' workgroup memory are decided in one
  place (`SHADPS4_LDS_BARRIERS=0/1` for any driver); Turnip does not need them (an Adreno runs
  64 threads in step, as GCN does).
- `vk_rasterizer.cpp`, `vk_presenter.cpp`: the draw dump and `SHADPS4_DBG_SYNC` (see
  [Looking into the picture](#looking-into-the-picture)).
- `host_passes/vr_overlay_pass.cpp`, `vk_presenter.cpp`, `known_title.cpp`: the headset's debug
  overlay (`SHADPS4_VR_OVERLAY`) and foveation's rings in colour (`SHADPS4_FOVEATION_SHOW`),
  drawn over the headset's frame with a built-in 8x16 font (made from DejaVu Sans Mono Bold);
  the title's frame time, scene size and GPU load are kept for it (`KnownTitle::GetStatus`).
- `src/video_core/...`: the mobile-GPU settings above, the pipeline cache warm-up skipped as
  under FEX on the Quest, and in `vk_platform.cpp` the OpenXR header included after the
  emulator's own (on Linux the Vulkan headers bring Xlib, whose `None` macro clashed).

The Quest app (`quest-host/`) is not used: it is an Android app (Java, OpenGL ES,
AHardwareBuffer, an Android sandbox for the emulator), none of which there is on SteamOS.

## Open questions

- **A higher display rate.** SteamVR on the Frame (2.18.2) offers programs only 72 Hz, so the
  game runs at 36 frames a second. At 90 or 120 Hz it could have 45 or 40; whether the game
  keeps that is another question: under FEX it would run at about 60% of its speed if left to
  itself, so the processor, not the GPU, is what limits the frame rate.
- **The Frame's own controller profile.** The runtime offers
  `XR_VALVE_frame_controller_interaction`; the emulator binds the Touch and Index profiles,
  which the Frame's controllers answer to. Binding the Frame's own would give its extra
  buttons a use.
- **A larger picture.** With foveation the GPU is busy about 60-80% of the time at 1440x1536;
  the emulator does not choose more than that by itself (`resolution=1600` and up asks for it).
